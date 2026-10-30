#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stddef.h>
#include <stdint.h>

#define EDVR_NATIVE_FRAME_VERSION_1 1u
// Version 2 adds the field-of-view trim to the END of EdvrNativeFrameOutput
// and nothing else. The two DLLs are shipped together but are copied apart
// by hand all the time, so the provider answers either version and a
// version 1 caller simply gets no trim.
#define EDVR_NATIVE_FRAME_VERSION_2 2u
// Version 3 adds deferredPacing to the END and nothing else. The same
// hand-copied-DLLs rule applies: the provider answers whichever of the three
// versions a caller's struct shape asks for, and a version 1 or 2 caller
// simply never turbo-paces, exactly as it never got a trim before version 2.
#define EDVR_NATIVE_FRAME_VERSION_3 3u
// Version 4 added one word to the END and nothing else, under the same
// hand-copied-DLLs rule (the projection channel of the terrain guard, retired
// 2026-10-09: the slot stays, see leftEyeShare below).
#define EDVR_NATIVE_FRAME_VERSION_4 4u
// Version 5 adds fadeAlpha to the END and nothing else (Explorer Cam's comfort
// fade, comfort_fade.h), under the same hand-copied-DLLs rule: a version 1 to
// 4 caller never learns of it, and a runtime that asks version 5 of a
// d3d11.dll that refuses the shape steps down and reads "no fade" (0), never
// black.
#define EDVR_NATIVE_FRAME_VERSION_5 5u
// Version 6 adds lookYawTotal and lookYawGameFrame to the END and nothing else (F22, look to aim: the body turn the runtime must hand back about
// the head so the world holds still). Same hand-copied-DLLs rule: a version 5 or older caller never learns of them, and a runtime that asks
// version 6 of a d3d11.dll that refuses the shape steps down and reads a total of 0 (no compensation).
#define EDVR_NATIVE_FRAME_VERSION_6 6u
// Version 7 (F31) is a ladder step, not an output field: the runtime asks for EdvrNativeFrameInput version 2 (the "now" head below) with the
// version 6 output shape. A provider that refuses it steps the runtime down to 6, which sends version 1 input. Same hand-copied-DLLs rule.
#define EDVR_NATIVE_FRAME_VERSION_7 7u
// Version 8 (F33, body align) adds eyeAlign and eyeAlignSerial to the END of the output, after version 7's ladder step: a seated-space translation the
// runtime folds into its seated origin. Same hand-copied-DLLs rule: a provider or runtime older than 8 reads zero (no alignment). Input stays version 2.
#define EDVR_NATIVE_FRAME_VERSION_8 8u
// Version 9 (F40, live A/B of the runtime's answer to Elite's "now" pose request, fix.now_pose) is version 8 with one word after it: nowPoseRender.
#define EDVR_NATIVE_FRAME_VERSION_9 9u
// Version 10 (F62b, the ADS head-aim, fix.native_on_foot_ads_head_aim) is a ladder step like 7: the runtime sends EdvrNativeFrameInput version 3
// (the head in natural LOCAL below) with the version 9 output shape, and a version 10 answer carries the ADS head-aim's seated pitch in the
// retired slot reservedYaw. A provider that refuses 10 steps the runtime down to 9 (input version 2, no ADS pitch). Same hand-copied-DLLs rule.
#define EDVR_NATIVE_FRAME_VERSION_10 10u
// Version 11 (world scale, visual.world_scale) is a ladder step like 10: the same shapes (input version 3, the whole output struct), and a version 11 answer carries
// the percent the runtime moves the eyes Elite is told about by, in worldScalePercent (the last of the retired guard's 21 words; layout and size unchanged). A
// provider that refuses 11 steps the runtime down to 10, which reads 100 (Elite's own scale). Same hand-copied-DLLs rule.
#define EDVR_NATIVE_FRAME_VERSION_11 11u
#define EDVR_NATIVE_FRAME_INPUT_VERSION_1 1u
#define EDVR_NATIVE_FRAME_INPUT_VERSION_2 2u
#define EDVR_NATIVE_FRAME_INPUT_VERSION_3 3u

// The game producer owns the device and generation passed at acquire. The
// methods in the table are CPU-only and are called by the XR owner after the
// producer has acquired the capability.
struct EdvrNativeFrameRequest {
    uint32_t size, version;
    ID3D11Device* gameDevice;
    uint64_t generation;
};

struct EdvrNativeFrameInput {
    uint32_t size, version;
    uint64_t generation, referenceGeneration, sequence;
    // Runtime pose, row-major 3x4. valid is zero when no pose was provided.
    float physicalHead[12];
    uint32_t valid;
    // Version 2 and later (F31). The head pose the runtime answered to Elite's "now" request most recently: the same 3x4 row-major rigid transform
    // as physicalHead (rigid(head.pose), in the seated space the render head is located in), with its answer count. The d3d11 half takes the
    // delta between this and physicalHead for the world markers. A version 1 input has none of these words (and must not be read for them).
    float nowHead[12];
    uint32_t nowValid;
    uint32_t nowSequence;
    // Version 2 and later (F37). The last eight render head poses the runtime handed the frame (rigid 3x4, as physicalHead), newest at ringNewest, with
    // their geometry sequences and QueryPerformanceCounter times. ringCount is how many are filled (at most 8). The d3d11 half matches the marker to them.
    float ringHead[8][12];
    uint64_t ringSequence[8];
    uint64_t ringQpc[8];
    uint32_t ringNewest;
    uint32_t ringCount;
    // Version 3 and later (F62b). The render head located in the runtime's natural LOCAL space at the frame's display time (the same 3x4 row-major
    // rigid transform as physicalHead), so it carries none of the seated origin's turns or offsets: the d3d11 half reads the headset's own
    // motion from it while the seated space is being turned under it. localValid is 1 when it was located.
    float localHead[12];
    uint32_t localValid;
    // F89 (was localReserved, always zero): the eye alignment the pose of this frame (physicalHead, located in the seated space) carries. 0: not reported (a runtime from
    // before F89, which sent zero here). Else the serial of the alignment (EdvrNativeFrameOutput::eyeAlignSerial, the one the runtime applied to its seated origin before it
    // located this pose) plus 1; 1 is "none applied" (serial 0). The d3d11 half takes exactly that alignment out of the pose when it measures the head's posture, matched by
    // serial and not by timing. A provider from before F89 ignores the word.
    uint32_t eyeAlignApplied;
};

// F101: an input version's size is the sizeof of the struct a caller of that version declares: its words, padded as sizeof pads them to the
// struct's alignment (8, for the uint64_t members). The words alone are not that size when the last word is not on an 8-byte boundary.
#define EDVR_NATIVE_FRAME_INPUT_PADDED(words) \
    ((uint32_t)(((words) + alignof(EdvrNativeFrameInput) - 1) & ~(alignof(EdvrNativeFrameInput) - 1)))
// The size of the version 1 input: the words through valid and the tail padding after them, which is what a version 1 caller's struct is (88 on
// x64: what v0.21.1 and every release before F31 sends, and the only size its provider accepts). F101: from F31 this was the words alone (84),
// so a released provider refused this branch's runtime and this branch's provider refused a released runtime. EDVR_NATIVE_FRAME_INPUT_WORDS_1
// is that unpadded size: the runtimes of this branch from F31 to F100 sent it, and a provider still takes it.
#define EDVR_NATIVE_FRAME_INPUT_WORDS_1 \
    ((uint32_t)offsetof(EdvrNativeFrameInput, nowHead))
#define EDVR_NATIVE_FRAME_INPUT_SIZE_1 \
    EDVR_NATIVE_FRAME_INPUT_PADDED(offsetof(EdvrNativeFrameInput, nowHead))
// F62b: the size of the version 2 input: the words through ringCount, which is what a version 2 caller's struct is (the LOCAL head follows it).
// F101: padded as version 1's is; ringCount ends on an 8-byte boundary (664 on x64), so the size is the words' own.
#define EDVR_NATIVE_FRAME_INPUT_SIZE_2 \
    EDVR_NATIVE_FRAME_INPUT_PADDED(offsetof(EdvrNativeFrameInput, localHead))

// F31: the render head and the "now" head the d3d11 half saw at its last beginFrame (nativeFrameNowPose). The frame sequence is the geometry
// sequence of that frame; nowSequence counts the runtime's "now" answers.
struct EdvrNativeNowPose {
    float render[12];
    float now[12];
    bool renderValid, nowValid;
    uint32_t nowSequence;
    uint64_t frameSequence;
    uint32_t inputVersion;   // the frame input version the provider last accepted (1 or 2)
    uint32_t referenceGeneration;   // the reference space's generation the frame was given (moves on a recentre, F33's tS0 reset)
    float ringHead[8][12];          // F37: the last eight render heads (version 2 input), newest at ringNewest
    uint64_t ringSequence[8];
    uint64_t ringQpc[8];
    uint32_t ringNewest, ringCount;
    float local[12];                // F62b: the render head in natural LOCAL (version 3 input), when localValid
    bool localValid;
    uint32_t eyeAlignApplied;       // F89: the version 3 input's applied-alignment word (0: not reported)
};

struct EdvrNativeFrameOutput {
    uint32_t size, version;
    // RETIRED 2026-10-07 with the old Explorer Cam route (a headset-pose offset
    // and yaw, applied while a key-counting gate said the external camera was
    // up). Always zero now: the provider writes nothing here and the runtime
    // reads nothing. The slots stay so the struct, and the four versions that
    // name its prefixes, keep their layout -- the two DLLs are copied apart by
    // hand, and a runtime from before the change then reads "offset off".
    // F61 (TEMPORARY, the rotation calibration, fix.native_on_foot_rot_calibrate): in a version 9 answer only, reservedOffset[0] and [1] carry
    // the seated rotation the runtime turns its seated origin by about the head: [0] the yaw about the vertical (radians, the seat's yaw sign),
    // [1] the pitch about the head's right axis (radians). Zero when the key is off. reservedOffsetEnabled stays 0, so a runtime from before
    // 2026-10-07 still reads "offset off"; a runtime older than F61 reads nothing here.
    // F62b (TEMPORARY, the ADS head-aim): in a version 10 answer only, reservedYaw carries the seated pitch (radians, nose up positive, at
    // most 60 deg either way) the runtime turns its seated origin by about the seat's right axis through the head. Zero when the feature is
    // off; a version 9 or older answer never writes it.
    float reservedOffset[3];
    float reservedYaw;
    uint32_t reservedOffsetEnabled, reservedOffsetGamePoses;
    // RETIRED 2026-10-09 with the terrain guard (a mode, three margins, a count
    // and up to eight headset signatures: 21 words). Always zero now: the provider
    // writes nothing here and the runtime reads nothing. The slots stay so the
    // struct, and the versions that name its prefixes, keep their layout --
    // the two DLLs are copied apart by hand, and a runtime from before the
    // removal then reads a guard that is off.
    // F123: the first of those 21 words is a request for a position-only recentre of the seated space, in a version 10 answer only: 0 when none stands, else the
    // request's serial (it moves on for each request, and the runtime acts once on each). The runtime moves the seated origin under the head, keeping its yaw (the
    // look compensation included) exactly, and never stores it as the seat. A runtime from before F123 reads a word it does not look at; a provider from before it
    // leaves it zero. Offset, size and type unchanged; the other 20 words are the retired guard's.
    uint32_t positionRecentre;
    // B3 (experimental.native_on_foot_ads_scope_zoom_full_detail, scope_zoom_window.h): in a version 11 answer only, the first five of these 19 words carry the scope zoom's window: the magic and the flags (the zoom's, and
    // in bits 4 and 5 what fills the left eye's strip: B3-2, scopewin::kEdge*), the narrowing M, the window's centre in the left eye's tangent plane (x, y), and how far the left eye's uncovered strip is
    // taken over by the fill (floats' bits). All zero when there is none; offset, size and
    // type unchanged, and the other 14 words are the retired guard's.
    uint32_t reservedTerrainGuard[19];
    // World scale (visual.world_scale, docs\world-scale.md), in a version 11 answer only: the percent the runtime moves the two eye origins Elite is told about by,
    // about their midpoint (50 to 200; 100 is Elite's own scale). Taken from the last of the retired guard's 21 words; offset, size and type unchanged. A provider
    // from before it leaves it zero and a runtime from before it never looks, and a zero or an answer older than 11 reads 100 (native_frame_client.h).
    uint32_t worldScalePercent;
    uint32_t sceneReady;
    uint32_t transitionEnabled;
    uint32_t resubmitEnabled;
    // Version 2 and later. Degrees off each eye's outer (temple), nasal
    // (nose) and vertical edges, 0..30, as a narrower projection and a
    // proportionally smaller render size.
    float trimOuterDeg, trimNasalDeg, trimVerticalDeg;
    // Version 3 and later. 1: hold the frame wait for the second Submit
    // (fix.weapon_stability while on foot); 0: the wait stays in
    // WaitGetPoses.
    uint32_t deferredPacing;
    // RETIRED 2026-10-09 (version 4's one word: which projection query channel
    // the terrain guard's widened frustum was told through). Always zero, never
    // read; the slot stays for the same reason as the 21 above.
    // F106 (fix.native_on_foot_ads_sight_eye = both): in a version 10 answer only, the slot carries the LEFT eye's share of the right eye's image, a float's bits
    // (left_eye_share.h): 0 (the left eye's own image) to 1 (the right eye's image, turned into the left eye's view). The runtime draws the right eye's image over the
    // left eye's own by it, in that eye's scene pass. F89 to F105 carried the left eye's darkening here, with the same timing: a runtime from F106 on reads such a
    // provider's level as the share, and a runtime from F89 to F105 darkens by this provider's share. A provider or runtime from before F89 leaves it zero and reads
    // nothing; a version 9 or older answer's slot is not believed (native_frame_client.h). Named reservedProjectionChannel until F106; offset, size and type unchanged.
    uint32_t leftEyeShare;
    // Version 5 and later. How black the projection layer is, 0 (clear, the
    // default and the only value an older provider can mean) to 1: the runtime
    // blends black over each eye image by this much when it composes. Already
    // freshness-checked by the provider (comfort_fade.h): a signal that went
    // stale reads 0.
    float fadeAlpha;
    // Version 6 and later (F22). The look-to-aim compensation total, in radians and the game's yaw sign: the body turn the game has made for our look
    // input so far. The runtime turns its seated origin about the head by the change since it last applied, so the reported head yaw drops by the
    // same amount and the world holds still. Cumulative (it never resets while the process runs); zero when the feature is off or not engaged.
    // lookYawGameFrame is the game frame index the total was taken at (the timing line compares it with the runtime's frame).
    float lookYawTotal;
    uint32_t lookYawGameFrame;
    // F25: a reset serial. Each time the controller disengages (the key goes off, the guard trips), the serial moves on; the runtime turns
    // its origin's compensation back to zero at once when it sees a new serial, and does not keep the total as a base.
    uint32_t lookYawReset;
    // Version 8 and later (F33). Seated-space metres the runtime moves its seated origin by (translation only, no yaw, no origin generation bump);
    // zero when the feature is off. eyeAlignSerial moves each time the provider takes a new value.
    float eyeAlign[3];
    uint32_t eyeAlignSerial;
    // Version 9 (F40, F42): the now-pose mode. 0 one display period ahead (next), 1 at the instant the frame draws at (render), 2 the latched
    // render pose of the frame itself (render_locked).
    uint32_t nowPoseRender;
};

// The size the fields through resubmitEnabled occupy, which is what a
// version 1 caller's struct is. Every member is four bytes, so this is the
// offset of the first version 2 field with no tail padding in play.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_1 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, trimOuterDeg))
// The size the fields through trimVerticalDeg occupy, which is what a
// version 2 caller's struct is: version 1's fields plus the three trims,
// with no tail padding before deferredPacing.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_2 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, deferredPacing))
// The size the fields through deferredPacing occupy, which is what a
// version 3 caller's struct is, with no tail padding before
// leftEyeShare (version 4's slot).
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_3 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, leftEyeShare))
// The size the fields through leftEyeShare (version 4's slot) occupy, which is what a version 4
// caller's struct is, with no tail padding before fadeAlpha.
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_4 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, fadeAlpha))
// The size the fields through fadeAlpha occupy, which is what a version 5 caller's struct is (F22: the look's two words follow it).
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_5 \
    ((uint32_t)offsetof(EdvrNativeFrameOutput, lookYawTotal))
// The struct through lookYawReset, which is what a version 6 caller's struct is (F33: the eye alignment follows it).
#define EDVR_NATIVE_FRAME_OUTPUT_SIZE_6 ((uint32_t)offsetof(EdvrNativeFrameOutput, eyeAlign))

struct EdvrNativeFrameDecision {
    uint32_t size, version;
    uint32_t withhold;
    uint32_t jumpOnly;
    uint32_t verdict;
};

typedef HRESULT(WINAPI *EdvrNativeFrameBegin)(
    void*, const EdvrNativeFrameInput*, EdvrNativeFrameOutput*);
// RETIRED 2026-10-09 with the terrain guard: this slot carried the guard's stage
// to the d3d11 half. Nothing calls it any more, and the provider's entry does
// nothing and returns S_OK, so a runtime built before the removal, which still
// checks that the slot is the provider's own, acquires as it always did.
typedef HRESULT(WINAPI *EdvrNativeFrameRetiredCall)(
    void*, uint32_t, float, float);
typedef HRESULT(WINAPI *EdvrNativeFrameLatchSubmit)(
    void*, uint64_t sequence, EdvrNativeFrameDecision*);
typedef HRESULT(WINAPI *EdvrNativeFrameInvalidate)(void*);
typedef HRESULT(WINAPI *EdvrNativeFrameClose)(void*);

struct EdvrNativeFrameTable {
    uint32_t size, version;
    void* context;
    EdvrNativeFrameBegin beginFrame;
    EdvrNativeFrameRetiredCall reservedCall;
    EdvrNativeFrameLatchSubmit latchSubmit;
    EdvrNativeFrameInvalidate invalidate;
    EdvrNativeFrameClose close;
};

extern "C" HRESULT WINAPI edvrAcquireNativeFrame(
    const EdvrNativeFrameRequest*, EdvrNativeFrameTable*);

namespace edvr {

// The worn headset's experimental.fov_trim_vertical/_outer/_nasal, resolved and
// cached by this DLL's own beginFrame (the same triple it hands the openvr
// half above): vertical, outer, nasal degrees, in that order. Takes
// native_frame.cpp's own g_mutex; zero in every slot until a beginFrame has
// resolved a headset (or when none is worn, e.g. under SteamVR/
// OpenComposite). For temporal_pass.cpp's fovea, which needs the same
// numbers in-process rather than across the DLL boundary above.
void nativeFrameFovTrimDegrees(uint32_t out[3]);

// The frame pairs the runtime has latched through latchSubmit, since the DLL loaded. Counted whatever the fixes; the mode watch
// samples it once a second (mode_watch.cpp).
uint64_t nativeFrameLatchedFrames();

// F22: the look-to-aim compensation the next beginFrame hands the runtime (version 6): the cumulative total in radians and the game frame it was
// taken at. Set by the frame thread once per frame (native_on_foot.cpp, lookPublishTick); read under the provider's lock by beginFrame.
void nativeFrameSetLookYaw(float total, uint32_t gameFrame, uint32_t resetSerial);
// F33 (body align): the seated-space translation (metres) the next version 8 beginFrame hands the runtime, and a serial that moves each time the
// frame thread takes a new value. Set by the d3d11 frame thread; read under the provider's lock by beginFrame.
void nativeFrameSetEyeAlign(const float offsetSeated[3], uint32_t serial);
void nativeFrameSetNowPose(uint32_t mode);   // F40/F42: the live now-pose mode (0 next, 1 render, 2 render_locked), carried in version 9 output
// F61 (TEMPORARY): the rotation calibration's seated yaw and pitch (radians, about the head), carried in version 9 output's reservedOffset[0..1].
// Zero when fix.native_on_foot_rot_calibrate is off. Set by the d3d11 frame thread; read under the provider's lock by beginFrame.
void nativeFrameSetRotCalibrate(float yawRad, float pitchRad);
// F62b (TEMPORARY): the ADS head-aim's seated pitch (radians, nose up positive), carried in version 10 output's reservedYaw. Zero when
// fix.native_on_foot_ads_head_aim is off or not engaged. Set by the d3d11 frame thread; read under the provider's lock by beginFrame.
void nativeFrameSetAdsPitch(float pitchRad);
// F123: the position-only recentre the frame thread asks the runtime for (0: none; else the request's serial), carried in a version 10 answer's positionRecentre. Set by the
// d3d11 frame thread once a frame; read under the provider's lock by beginFrame.
void nativeFrameSetPositionRecentre(uint32_t serial);
// F115: the on-foot words the next beginFrame hands the runtime, as the frame thread last set them (the boarding reset reads them back: all zero from the edge
// on). Taken under the provider's lock.
struct EdvrNativeOnFootWords {
    float eyeAlign[3];
    uint32_t eyeAlignSerial;
    float lookYawTotal;
    uint32_t lookYawReset;
    float adsPitch;
    float rotCal[2];
};
void nativeFrameOnFootWords(EdvrNativeOnFootWords& out);
// F31: the latest frame's render head and "now" head (false until a beginFrame has been received). Read on the game thread.
bool nativeFrameNowPose(EdvrNativeNowPose& out);

// World scale: the percent the last beginFrame told the runtime to move the eyes Elite is told about by (50 to 200), or 100 when no beginFrame has answered a
// version 11 ask (no runtime, or one that asks less, which applies none). What Elite's eye pair measures is the located separation times
// worldScaleEyeFactor() of this (common/world_scale.h), so every gate on that pair takes its bounds from it. Lock-free.
uint32_t nativeFrameWorldScale();

// F101: what the runtime sends for each step of its version ladder (native_frame_client.h), here so the provider's rig drives the same shapes.
// The output a version asks with: the whole struct for 8 to 11, each older version's own prefix below.
inline EdvrNativeFrameOutput nativeFrameAsk(uint32_t version) {
    if (version >= EDVR_NATIVE_FRAME_VERSION_8)
        return EdvrNativeFrameOutput{sizeof(EdvrNativeFrameOutput), version >= EDVR_NATIVE_FRAME_VERSION_11 ? EDVR_NATIVE_FRAME_VERSION_11 :
                                     version >= EDVR_NATIVE_FRAME_VERSION_10 ? EDVR_NATIVE_FRAME_VERSION_10 :
                                     version >= EDVR_NATIVE_FRAME_VERSION_9 ? EDVR_NATIVE_FRAME_VERSION_9 : EDVR_NATIVE_FRAME_VERSION_8};
    if (version >= EDVR_NATIVE_FRAME_VERSION_6) return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_6, EDVR_NATIVE_FRAME_VERSION_6};
    if (version == EDVR_NATIVE_FRAME_VERSION_5) return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_5, EDVR_NATIVE_FRAME_VERSION_5};
    if (version == EDVR_NATIVE_FRAME_VERSION_4) return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_4, EDVR_NATIVE_FRAME_VERSION_4};
    if (version == EDVR_NATIVE_FRAME_VERSION_3) return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_3, EDVR_NATIVE_FRAME_VERSION_3};
    if (version == EDVR_NATIVE_FRAME_VERSION_2) return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_2, EDVR_NATIVE_FRAME_VERSION_2};
    return EdvrNativeFrameOutput{EDVR_NATIVE_FRAME_OUTPUT_SIZE_1, EDVR_NATIVE_FRAME_VERSION_1};
}
// The input shape that rides an ask of that version: input version 3 (the whole struct) with 10 and 11, version 2 (the words through ringCount) with 7 to
// 9, version 1 (the words through valid, padded as the released struct is) below 7. Only size and version change: a provider reads no word past
// the shape it was told.
inline void nativeFrameInputShape(uint32_t version, EdvrNativeFrameInput& in) {
    if (version < EDVR_NATIVE_FRAME_VERSION_7) {
        in.size = EDVR_NATIVE_FRAME_INPUT_SIZE_1;
        in.version = EDVR_NATIVE_FRAME_INPUT_VERSION_1;
    } else if (version < EDVR_NATIVE_FRAME_VERSION_10) {
        in.size = EDVR_NATIVE_FRAME_INPUT_SIZE_2;
        in.version = EDVR_NATIVE_FRAME_INPUT_VERSION_2;
    } else {
        in.size = sizeof(EdvrNativeFrameInput);
        in.version = EDVR_NATIVE_FRAME_INPUT_VERSION_3;
    }
}

}  // namespace edvr

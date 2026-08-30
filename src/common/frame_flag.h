// A one-bit channel between the two proxies, for the frame that must not be
// shown.
//
// The bad frame is detected in d3d11.dll, part-way through rendering, by
// watching where the game is drawing from. The decision it feeds -- whether to
// hand that frame to the headset -- belongs to openvr_api.dll. Those are
// separate modules loaded from different directories, so they cannot share a
// global.
//
// A named shared mapping is used rather than an exported symbol because it does
// not care which module loads first, works if one of them is absent, and adds
// nothing to either DLL's export table. It is per-process and holds two
// integers.
//
// Nothing here reads or writes game state. It carries one flag between two parts
// of EDVR.
#pragma once

#include <cstdint>

namespace edvr {

// Mark the frame in progress as one that should not reach the headset. Cheap and
// safe to call from the draw path.
void markGlitchFrame();

// True if the frame in progress has been marked.
bool glitchFrameMarked();

// Withdraw a mark WITHIN the frame that made it.
//
// The detector re-decides several times per frame, as each new camera candidate
// arrives, and an early candidate can look like a jump while the frame's final
// verdict is no.
//
// Identical to clearGlitchFrame() today, and kept as its own name because the
// two mean different things to a reader: one is "I was wrong", the other is
// "this frame is over". They used to differ -- a `counted` field distinguished
// them, so a withdrawn mark would not be tallied twice -- but that total moved
// into glitch_frame.cpp and the field became write-only. It has been removed
// rather than left as a mechanism the header describes and nothing implements.
void unmarkGlitchFrame();

// Called once per frame, so a mark applies to exactly one frame. Without this a
// single detection would suppress every frame that followed.
void clearGlitchFrame();

// The submitted eye textures, one slot per eye (0 left, 1 right), raw
// ID3D11Texture2D pointers valid within this process. Written by
// openvr_api.dll at each Submit; read by d3d11.dll's FSS series at the
// frame boundary. Null means nobody has published (no openvr proxy, or a
// mismatched pair), and readers fall back exactly as if unhooked.
void publishSubmitTexture(int eye, void* texture);
void* submittedTexture(int eye);

// The game's own ID3D11Device, published by d3d11.dll the moment the game
// creates it. A raw pointer valid within this process, like submitTex above.
//
// This one runs the other way round from every other d3d11 -> openvr field:
// it is published BEFORE the openvr half has been called at all. Measured on
// the field rig, the device is created 1.20 s before openvr_api.dll is first
// asked for an interface, which is the whole reason the early handover has
// anything to work with (early_session.h).
//
// Null means no d3d11 half, or a device the proxy never saw. Every reader
// must treat that as "stand down", not as a reason to wait.
void  publishGameDevice(void* device);
void* gameDevice();

// The FSS arrival-mono window: d3d11 sets the frame count when a camera
// jump lands while the scanner's screen is up; the openvr half submits
// the right eye's texture for both eyes while it counts down (one
// decrement per frame, its own call so the reader cannot double-count a
// frame with two submits).
void setFssMonoFrames(int n);
int  fssMonoRemaining();
void decFssMonoFrames();

// The scanner-chrome stamp: bumped by d3d11 on every frame the scanner's
// screen is drawn; the openvr half judges staleness against its own frame
// count. The eye heal's gate.
void bumpFssChromeStamp();
long fssChromeStampValue();

// The scanner-BODY stamp: bumped by d3d11 on every frame the fully-zoomed
// body layer draws. The theater's gate -- the final zoom, and only it.
void bumpFssBodyStamp();
long fssBodyStampValue();

// The scanner screen's PROJECTED CORNERS in the eye -- TL,TR,BR,BL as
// (u,v) pairs, 8 floats -- derived by d3d11 once per theater engage. The
// openvr half hands them to the renderer, which rectifies the quad
// through a square-to-quad homography: the content arrives level and
// fully framed whatever the screen's tilt or the head's pose at engage.
void publishFssPanelRect(const float* corners16);   // both eyes, L then R
long fssPanelRectSeqValue();
bool readFssPanelRect(float* out16);

// The centring servo: the openvr half rotates the frozen pose toward
// square-on and bumps this to request a fresh derivation.
void bumpFssPanelRectRedo();
long fssPanelRectRedoValue();

// The zoom-press arrival window, one bump per open frame: the squares'
// ~10 frames, and the only frames the window-scoped heal touches.
void bumpFssArrivalStamp();
long fssArrivalStampValue();

// A camera-jump latch WITHIN d3d11.dll (plain process state, not the
// mapping): the glitch detector notes every world-camera jump; the
// arrival census takes the latch at the frame boundary. Lives here so
// the standalone glitch test, which links this file, resolves it.
void noteWorldJump();
bool takeWorldJump();

// Announced by openvr_api.dll once its hook is validated, and read by d3d11.dll.
//
// The two halves install separately and the openvr one is optional, so d3d11 can
// detect a bad frame and have nothing on the other end to act on it. Without
// this the log would report "12 frames withheld" to somebody who skipped the
// second file and had none withheld at all -- which is exactly the kind of
// counter that gets believed and wastes a day.
void announceGlitchConsumer();
bool glitchConsumerPresent();

// The player is on foot in the external camera, having arrived there from the
// flat panel -- the one state where moving the head pose is wanted.
//
// Set by d3d11.dll, which is the only half that can tell the modes apart: it
// watches the panel composite, and the panel stopping while a full scene is
// drawn into the eyes is what the transition looks like. Read by
// openvr_api.dll, which is the only half that can act on it, because the head
// pose passes through there. Neither can do the other's job, which is why this
// is a channel rather than a local.
//
// UNLIKE the glitch flag, this one is a STATE and persists across frames. It is
// not cleared at the frame boundary; it is cleared when the panel comes back.
// Call this EVERY frame, with the current answer, not only when it changes. The
// repetition is the point: it is also the heartbeat that externalCameraOnFootLive
// reads, so "d3d11 says no" and "d3d11 has stopped saying anything" stay
// distinguishable.
void setExternalCameraOnFoot(bool on);

// The last value written, whenever it was written.
//
// Prefer externalCameraOnFootLive for anything that MOVES THE PLAYER. This one
// cannot tell a current "yes" from a "yes" left behind by a writer that has
// since stopped, and the header used to say as much without doing anything
// about it: "a wrong answer here does not expire on its own".
bool externalCameraOnFoot();

// True only if d3d11.dll says yes AND has said something within maxAgeFrames
// calls of this function.
//
// WHY A HEARTBEAT. The writer runs inside a fault-budgeted SEH guard on the
// Present path, and that guard stops running its body permanently after a few
// faults. The gate would then freeze at whatever it last published. Frozen ON
// means the head offset stays applied in every mode -- including the cockpit --
// for the rest of the session, with both logs still saying it is gated, because
// no line is printed for a decision that is never re-made. The same freeze
// follows from the d3d11 hook being lost to a recreated device or swapchain.
//
// This is the argument the openvr half already makes for keeping the offset
// itself outside its own budgeted guard: a fault budget is right for logging,
// where losing a line costs nothing, and wrong for anything that changes what
// the player sees. The writer here cannot move out of its guard -- it is
// derived from the render state the guard exists to inspect -- so the reader
// stops trusting it instead.
//
// Counted in READER frames rather than time. Both halves run once per rendered
// frame, so the units match without a clock, and a stall that freezes both
// halves together does not age the flag while nothing is being drawn anyway.
bool externalCameraOnFootLive(uint32_t maxAgeFrames);

// Has ANYTHING ever published the mode, whatever it said?
//
// "Nobody is publishing" and "the publisher says no" are different facts and a
// reader that cannot tell them apart will accuse a healthy install of being
// broken. That is not hypothetical: the openvr side warned "nothing has
// reported the player's mode" on every correctly-configured session about forty
// seconds in, because until the player first enters the camera the gate is
// publishing "no" continuously and the two look identical from here.
bool externalCameraEverPublished();

// Ask the openvr half to decline the next `frames` frames.
//
// NOT A DETECTION, and that is the whole point of it. Everything else across
// this channel is one half telling the other what it inferred; this is the
// player saying so. They pressed the external-camera key, so a transition is
// starting -- there is nothing to recognise and nothing to be wrong about.
//
// It exists because during that transition Elite draws several frames from
// somewhere the player is not -- confirmed with fix.transition_flash = 0, so it
// is the game's and not ours -- and detection cannot help: withholding shows the
// PREVIOUS frame, and by the time a detector has recognised a bad one, the
// previous frame is already bad too. Starting at the press means the frame being
// held is the last good one before any of it.
//
// The cost is unmeasured and is the reason this ships off by default. One
// withheld frame has measured 74-82 ms because the compositor waits for a submit
// that never comes; whether a RUN of them costs that each or settles into steady
// reprojection is not known, and the difference is between a smooth hold and a
// freeze. The ring's timing column is the readout.
void requestSubmitHold(uint32_t frames);

// One frame of that hold, consumed by the reader. True while the hold is live.
bool takeSubmitHoldFrame();

// The size of the texture the game hands the headset, as openvr_api.dll read it
// off the Submit argument.
//
// WHY THIS IS A CHANNEL AND NOT A CONSTANT. The d3d11 half has to decide, per
// render target, whether it is one of the eyes -- everything downstream of that
// answer (the black void, the panel distance, the transition-flash detector's
// "is a scene being drawn", and the head-offset gate) is fed by it. It cannot
// see a Submit, so it guessed by size: 2048x2048 or larger. A guess is what the
// openvr half never has to make, because the texture is handed to it by name.
//
// Two failures came of the guess, and both are silent by construction -- a
// target that is not recognised produces no line, because nothing happened.
// A headset whose eye textures are under 2048 on an axis is never recognised at
// all. And a panel raised to a size that is ALSO 2048-or-larger has to be told
// apart from the eyes, which was done by size too -- so a vscreen_res that
// happens to equal the eye textures excluded the eyes along with the panel.
//
// Published as one packed value rather than two fields on purpose: a reader
// that catches a half-written pair gets a width from this session and a height
// from the last one, and the eye test is an equality test. Width and height are
// each under 65536 for any headset that exists, so both fit in one LONG and the
// exchange is atomic.
void announceEyeTextureSize(uint32_t width, uint32_t height);

// The eye-texture size, or false when nobody has published one -- openvr_api.dll
// is not installed, its hook has not validated yet, or the game submits
// something that is not a D3D11 texture. False means "no answer", never "no":
// callers must fall back to what they did before this existed rather than treat
// it as evidence about any particular target.
bool eyeTextureSize(uint32_t* width, uint32_t* height);

// The headset's true horizontal frustum, published by openvr_api.dll as it
// observes GetProjectionRaw: the OUTER (temporal) and INNER (nasal) tangent
// magnitudes of one eye. The eyes mirror on every headset measured, so one
// pair describes both.
//
// WHY THIS IS A CHANNEL: the RemLok overlay fix places the helmet's edge
// line at a chosen ANGLE from straight ahead, and the tuned fractions that
// looked right on two different headsets turned out to be one angle wearing
// two denominators (0.70 of a Quest 3's image and 0.60 of a Pimax's both
// put the line within a degree of 46) -- the angle is the human constant,
// the tangents are the per-headset denominator, and only the openvr half
// can read them. Same discipline as eyeSize: packed into one word so a
// reader cannot tear the pair, zero is "no answer", and a reader without an
// answer falls back to the manual scale.
void announceEyeTangents(float outerMag, float innerMag);
bool eyeTangents(float* outerMag, float* innerMag);

// The VERTICAL frustum of one eye, magnitudes of the top and bottom
// tangents. Both eyes share these -- measured identical on every headset
// seen -- so there is one pair, not two.
//
// This exists because the vertical span used to be DERIVED, from the
// horizontal span and the eye texture's shape, on the assumption that the
// vertical frustum is symmetric. That assumption was checked against one
// headset and holds there exactly:
//
//   Pimax Crystal Super  5424x5356  t=-1.2648 b=+1.2648   symmetric
//   Quest 3 (Virtual Desktop)
//                        3072x3264  t=-1.4281 b=+0.9657   NOT symmetric
//
// On the Quest 3 the derivation puts the frustum's vertical centre 0.19 of
// a half-height out, and because the intro panel is world-locked that error
// is applied through a projection that no longer matches the runtime's --
// so the panel shears as the head moves. Reported from the field and
// reproduced (docs/intro-video.md).
//
// Publishing the measurement instead of deriving it is the fix. Readers
// must still handle false: an older openvr_api.dll publishes nothing here,
// and the derivation remains as the fallback for exactly that pairing.
void announceEyeTangentsVertical(float topMag, float botMag);
bool eyeTangentsVertical(float* topMag, float* botMag);

// Where the ship's forward axis points in the CURRENT head frame, as
// tangent-space offsets from straight ahead -- published by openvr_api.dll
// every frame from the pose it is handed, read by d3d11.dll to counter-move
// head-locked sprites so they hold a direction instead of riding the head.
// (0,0) means "looking dead ahead" and is a legitimate value, so the packing
// carries an always-set presence bit: a zero word is "nobody publishing",
// never "centred".
void announceHeadForward(float tx, float ty);

// The head pose the runtime returned, row-major 3x4 (rotation in the left
// 3x3, translation in the last column), published by openvr_api.dll every
// frame BEFORE any EDVR offset touches it.
//
// headForward above is a DIRECTION and cannot carry roll or translation,
// which is enough to counter-move a sprite and not enough to place a panel
// in the world. The intro movie needs the whole pose: it builds a real
// view-projection so the movie is drawn on the splash's own screen, with
// the stereo that a screen at a distance has (intro_panel.h). False when
// nothing has published, which the caller must treat as "leave it alone".
void publishHeadPose(const float* m12);
bool headPose(float* out12);
bool headForward(float* tx, float* ty);

// The cull guard's state, published by openvr_api.dll at its stage
// transitions and read by d3d11.dll once per frame boundary.
//
// WHY THE DETECTOR NEEDS IT (SPEC-FLASH-FALSE-POSITIVES §1g, EVIDENCE 6bp):
// the guard tells the game a wider frustum than the headset shows, the wider
// frustum admits more near-surface render passes, and the transition-flash
// detector's recognition machinery churns in proportion -- 29 recognitions at
// guard-off against 3,277 at half margin, with the learning tax felt as
// judder. Whether that churn is table thrash, genuine novelty, or an
// identifiable camera population is exactly what nobody has measured, so this
// channel carries ATTRIBUTION: the detector stamps its ring and splits its
// counters by what the guard was doing, and changes no decision on it.
//
// The eyeSize disciplines apply unchanged. One packed value, so a reader
// cannot catch half a pair. Zero means "no answer", and every reader must
// treat it exactly like guard-off -- openvr_api.dll absent, its guard never
// armed, or a mismatched build pair (the mapping version isolates those) all
// look identical, and all of them are states in which no lie is being told.
//
// stage is 0 (off), 1 (the game is asked for BIGGER render targets but still
// told true projections -- supersampling only), or 2 (the projection lie is
// live). The factors are the per-axis span ratios lied/true, carried as
// per-mille above 1.0 -- +6.1% renders as 61. Stage 1 is published
// distinctly on purpose: it changes pixel count but not the reported
// frustum, so detector churn moving at stage 1 alone would be a finding
// about resolution-dependent pass composition, not noise.
void announceCullGuardState(uint32_t stage, float factorH, float factorV);

// The packed value as last published, or 0 for "no answer". Packing, also
// relied on by decodeCullGuardState below: bits 25..24 stage, 23..12
// horizontal per-mille, 11..0 vertical per-mille.
uint32_t cullGuardStatePacked();

// The unpacked reading. Header-only and pure, like SubmitPairLatch and for
// the same reason: both halves and every test decode one way.
struct CullGuardState {
    uint32_t stage;      // 0 off, 1 size-only, 2 lie live
    uint32_t hPerMille;  // (span ratio - 1) * 1000, horizontal
    uint32_t vPerMille;
};
inline CullGuardState decodeCullGuardState(uint32_t packed) {
    CullGuardState s;
    s.stage = (packed >> 24) & 0x3u;
    s.hPerMille = (packed >> 12) & 0xFFFu;
    s.vPerMille = packed & 0xFFFu;
    return s;
}

// ONE VERDICT PER FRAME, over a channel that carries no frame identity.
//
// The mark above is read once per eye, at each Submit, and it legitimately
// changes DURING a frame: the detector re-decides on every new furthest camera
// and can set, withdraw and re-raise it by design. So a transition landing
// between Submit(left) and Submit(right) shows one eye this frame and the other
// a reprojection of the last one -- a one-frame binocular mismatch, which is
// what a flash feels like. The fix for flashes, producing one.
//
// It cannot be solved by comparing frame numbers, because this channel does not
// carry one (EDVR-31). It is solved by sampling once and making the second eye
// follow, which is available without any protocol change at all.
//
// CONSISTENT-LATE BEATS SPLIT, in both directions. Two eyes showing the previous
// frame is a dropped frame, which runtimes reproject and people are used to. Two
// eyes disagreeing is not something either handles.
//
// Header-only and pure, so the property can be asserted without a compositor --
// and shared, so the two copies of the hook cannot drift. compositor_hook.cpp is
// FORKED and its "kept aligned by hand" regions have no mechanical check, which
// is the standing gap in the sync tooling; this is four lines fewer to keep
// aligned by remembering.
class SubmitPairLatch {
public:
    // The verdict for this frame. The first call after reset() decides it; every
    // later call in the same frame gets the same answer, whatever the flag has
    // done since.
    bool verdict(bool markedNow) {
        if (!m_latched) {
            m_latched = true;
            m_withhold = markedNow;
        }
        return m_withhold;
    }

    // Called at the frame boundary -- WaitGetPoses, which is where the mark
    // itself is cleared.
    void reset() {
        m_latched = false;
        m_withhold = false;
    }

    bool latched() const { return m_latched; }

private:
    bool m_latched = false;
    bool m_withhold = false;
};

}  // namespace edvr

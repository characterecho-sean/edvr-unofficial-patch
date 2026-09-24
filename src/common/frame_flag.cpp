#include "frame_flag.h"

#include <windows.h>

#include <cstdio>  // _snwprintf_s

namespace edvr {
namespace {

// flag      the frame in progress is marked
// consumer  openvr_api.dll has a live Submit hook and can act on the flag
//
// There was a third field, `counted`. Nothing ever read it: it was written by
// markGlitchFrame and reset by clearGlitchFrame, and the session total it once
// fed moved into a local counter in glitch_frame.cpp long ago. Removed along
// with the header's explanation of a mechanism that no longer exists.
struct Shared {
    volatile LONG flag;
    volatile LONG consumer;
    // externalCam  the player is on foot in the external camera, having come
    //              there from the flat panel
    //
    // Same shape of problem as `flag` and so the same channel: d3d11.dll is the
    // only half that can tell the modes apart -- it watches the panel composite
    // -- and openvr_api.dll is the only half that can act on the answer, because
    // the head pose passes through it. Neither can do the other's job.
    volatile LONG externalCam;
    // externalCamStamp  bumped on every write of externalCam, including the
    //                   writes that do not change it
    //
    // externalCam alone cannot distinguish "d3d11 says no" from "d3d11 has
    // stopped saying anything", and the difference decides whether a player's
    // viewpoint is still being moved in the cockpit. The writer sits inside a
    // fault-budgeted guard that stops running permanently after a few faults,
    // so "stopped saying anything" is a reachable state and not a theoretical
    // one.
    //
    // A counter rather than a timestamp: no clock, no wraparound handling worth
    // the name (2^32 frames is over a year at 90 Hz), and it compares with a
    // plain !=.
    volatile LONG externalCamStamp;
    // holdFrames  frames the openvr half should decline to submit, counting
    //             down, set by d3d11 when the player presses a key that starts
    //             a transition
    //
    // NOT a detection. Every other route in this file is one half telling the
    // other what it INFERRED; this is the player telling us directly. They
    // pressed the external-camera key, so a transition is starting -- there is
    // nothing to detect and nothing to get wrong about which mode we are in.
    //
    // It exists because during that transition Elite draws several frames from
    // somewhere the player is not, and no amount of detection helps: withholding
    // shows the PREVIOUS frame, and by the time a detector has recognised a bad
    // frame the previous one is already bad too. Starting the hold at the press
    // means the frame being held is the last good one before any of it.
    //
    // Separate from `flag` because `flag` is cleared at every WaitGetPoses by
    // design -- one detection must not suppress the frames after it -- and this
    // is the opposite: a deliberate run, counted down by the reader.
    volatile LONG holdFrames;
    // eyeSize  the width and height of the texture submitted to the headset,
    //          packed as (width << 16) | height, written by openvr_api.dll
    //
    // The direction of every other field in here is d3d11 -> openvr. This one
    // runs the other way, and for the mirror-image reason: the openvr half is
    // handed the eye texture and the d3d11 half was reduced to guessing which
    // of the render targets it sees is one. See the header.
    //
    // Packed into one field so a reader cannot catch half of a pair. Zero means
    // nobody has published, which is a state the reader must handle: the openvr
    // proxy is optional and this is written only after its hook validates.
    volatile LONG eyeSize;
    // submitTex  the two textures most recently handed to Submit, one slot
    //            per eye, written by openvr_api.dll at each Submit
    //
    // The third openvr -> d3d11 field family. The FSS series needs to
    // measure EXACTLY what reaches the headset, and the d3d11 half owns
    // every tool for doing that (compute reduction, staging, the log) but
    // was reduced to guessing WHICH texture is submitted -- a guess that
    // broke twice in one day when the pipeline reshaped. Raw pointers are
    // valid across the two halves because they are one process; a 64-bit
    // aligned volatile store is atomic on x64, so no reader tears one.
    // Zero is "nobody has published", the eyeSize discipline.
    volatile LONG64 submitTex[2];
    // fssChromeStamp  bumped by d3d11 on every frame that draws the
    //                 scanner's chrome -- externalCamStamp's discipline:
    //                 a counter, compared with !=, staleness judged by
    //                 the reader against its own frame count. The eye
    //                 heal's gate.
    volatile LONG fssChromeStamp;
    // The scanner screen's rectangle in the (left) eye, derived by d3d11
    // once per theater engage from the composite's own constants; the
    // openvr half crops the cinema screen's content to it. seq bumps per
    // publish; 0 means never published.
    volatile LONG fssPanelRectSeq;
    // The arrival stamp: bumped by d3d11 each frame the zoom-press
    // window is open. The heal scopes itself to exactly these frames.
    volatile LONG fssArrivalStamp;
    // The head pose the runtime returned, row-major 3x4, published every
    // frame BEFORE any EDVR offset touches it. The intro panel builds a
    // world-space transform from it so the movie can be drawn on the
    // splash's screen instead of pasted to the face (intro_panel.h). seq is
    // the presence bit: a never-published channel and a genuinely zero pose
    // would otherwise read the same.
    volatile LONG headPoseSeq;
    float         headPoseM[12];
    float         fssPanelRect[16];  // corner UVs TL,TR,BR,BL as (u,v):
                                     // [0..7] the LEFT eye's, [8..15] the
                                     // RIGHT eye's -- the renderer
                                     // stitches the two images, each
                                     // clean on its temporal side
    // cullGuard  the cull guard's stage and margin, packed as
    //            (stage << 24) | (hPerMille << 12) | vPerMille, written by
    //            openvr_api.dll at its stage transitions
    //
    // The second openvr -> d3d11 field, and eyeSize's disciplines carry over
    // whole: one packed word so no reader tears a pair, zero is "no answer"
    // and must be read as guard-off, and a mismatched build pair is made
    // inert by the mapping version rather than subtly wrong by the layout.
    // What it exists for -- attribution of detector churn to the guard's
    // margin, never a decision -- is documented at the header declaration
    // and in SPEC-FLASH-FALSE-POSITIVES §1g.
    volatile LONG cullGuard;
    // eyeTangents  the true horizontal frustum of one eye, packed as
    //              (outerMilli << 16) | innerMilli -- tangent magnitudes
    //              times 1000 -- written by openvr_api.dll as it observes
    //              GetProjectionRaw
    //
    // The third openvr -> d3d11 field, same disciplines. What it exists
    // for -- deriving the per-headset overlay scale that puts the RemLok
    // line at a chosen angle -- is documented at the header declaration.
    volatile LONG eyeTangents;
    // eyeTangentsV  the true VERTICAL frustum, packed as
    //               (topMag_milli << 16) | botMag_milli. Both eyes share
    //               it -- measured identical on every headset seen. Zero
    //               is "nobody published", and the reader falls back to
    //               DERIVING it, which is right on a symmetric headset
    //               and wrong on a Quest 3. See frame_flag.h.
    volatile LONG eyeTangentsV;
    // headForward  ship-forward in the current head frame, tangent-space,
    //              packed as (1 << 31) | ((tx_milli + 16384) << 15) |
    //              (ty_milli + 16384), each biased-15-bit, tangents times
    //              1000 clamped to +/-3.0 -- written by openvr_api.dll
    //              every frame from the pose it hands the game
    //
    // The fourth openvr -> d3d11 field. Biased rather than raw because
    // straight-ahead is (0,0) and a legitimate publication, while a zero
    // WORD must keep meaning "nobody publishing" -- the presence bit and
    // the bias keep every published value nonzero.
    volatile LONG headForward;
    // gameDev  the game's own ID3D11Device, written by d3d11.dll at device
    //          creation. Read by openvr_api.dll's cull guard as the test for
    //          a d3d11 half being installed at all.
    //
    // The one field published before the openvr half has run at all. It
    // joined for the early VR handover (removed 2026-09-13), which needed a
    // texture on the game's device before the game asked for the compositor;
    // the slot stays, because the channel layout is versioned and a presence
    // test still reads it.
    //
    // LONG64 rather than LONG: this is a 64-bit pointer, and submitTex above
    // is the precedent.
    volatile LONG64 gameDev;
    // sceneArrived  latched by d3d11 at its scene boundary; read by the
    //               cull guard so it does not lie about the frustum while
    //               the intro is still on screen. See frame_flag.h.
    volatile LONG sceneArrived;
    // The settings menu (docs/settings-menu.md): the anchor pose the panel
    // was summoned at (d3d11 -> openvr, headPose's layout, seq as presence
    // and change stamp), the per-frame visibility heartbeat with the fade
    // alpha in per-mille (d3d11 -> openvr), and the drawn counter the
    // keyboard gate follows (openvr's draw, bumped by the d3d11 export).
    volatile LONG menuAnchorSeq;
    float         menuAnchorM[12];
    volatile LONG menuAlphaMille;
    volatile LONG menuVisibleStamp;
    volatile LONG menuDrawn;
    // The overlay's head lock: bit 31 on, then yaw and pitch as tenths of a
    // degree, each biased into twelve bits (kHeadLockBias).
    volatile LONG     menuHeadLock;
    // The detector's verdict on the last jump, d3d11 -> openvr: bits 0-1 say
    // whether the camera came back (1, a glitch) or stayed (2, a change of
    // reference frame), and the bits above them count the verdicts, so a
    // reader that remembers the value at a withhold can tell a NEW verdict
    // from the last jump's. One word, so the two never tear.
    volatile LONG     jumpVerdict;
    // The runtime-supplied hidden-area mesh's triangle count per eye,
    // openvr -> d3d11, headForward's packing (presence bit, two biased fields): see
    // announceRuntimeMaskTriangles in frame_flag.h.
    volatile LONG     runtimeMaskTri;
    // introRecentre  d3d11 -> openvr, requestIntroRecentre's one-shot ask:
    //                nonzero means "recentre the seated origin to the
    //                current head pose", taken (cleared) by the vr half's
    //                own poll. See frame_flag.h.
    volatile LONG     introRecentre;
    // advanced.eye_origin_readers (docs/design-transition-flash-engine-fix-
    // 2026-09-23.md, part A1): d3d11 -> openvr, the request to start
    // capturing the runtime's own WaitGetPoses/GetLastPoses call stack.
    volatile LONG     poseReaderRequest;
    // The rest of this family runs openvr -> d3d11, published at EVERY
    // WaitGetPoses/GetLastPoses call: PoseReaderCall's fields (frame_flag.h)
    // packed the same way submitTex/gameDev use LONG64 for a pointer. seq is
    // bumped last, so a reader who samples it before and after a read can
    // tell a torn snapshot from a fresh one (it never blocks on it -- this
    // is a diagnostic, not a lock).
    volatile LONG     poseReaderSeq;
    volatile LONG64   poseReaderRenderPtr;
    volatile LONG64   poseReaderGamePtr;
    volatile LONG64   poseReaderQpc;
    volatile LONG     poseReaderRenderCount;
    volatile LONG     poseReaderGameCount;
    volatile LONG     poseReaderThreadId;
    // bit0 render ptr on the calling thread's stack, bit1 game ptr on it,
    // bit2 this publish came from GetLastPoses rather than WaitGetPoses.
    volatile LONG     poseReaderFlags;
};

// Per PROCESS, not per logon session.
//
// This said Local\edvr_glitch_frame_v1 under a comment claiming it was scoped
// "so two copies of the game do not share one flag". Local\ is the per-logon
// BaseNamedObjects namespace: every process one user is running shares it. Two
// Elite clients -- a normal thing for multi-account play -- therefore shared a
// single flag, and each one's per-frame clear wiped the other's mark before it
// could be read. One client's flash was shown anyway, the other withheld a good
// frame, and the d3d11 half of a client with no openvr proxy installed at all
// would report "withheld" because the OTHER process had announced itself.
//
// The name is built once, at first use. The two DLLs are in the same process,
// so the channel between them is unaffected.
//
// _v35 because the pose-reader hunt joined (poseReaderRequest and the
// poseReader* call snapshot), for advanced.eye_origin_readers (docs/design-
// transition-flash-engine-fix-2026-09-23.md). Two branches each took _v34
// for different layouts on 2026-09-23; the merge of both is _v35.
// _v34 because the channels only the legacy openvr half ever wrote left
// the layout -- fssMonoFrames, fssBodyStamp, fssPanelRectRedo, the gaze,
// the compositor's frame timing and EDVR's activity words -- nothing had
// written them since that proxy was deleted; and because the halves now
// sign the roll-call below, so the next mismatch is refused aloud.
// _v33 because the intro panel's recentre request joined (introRecentre),
// for the seated-origin fix in docs/intro-video.md, 2026-09-17.
// _v32 because runtimeKind left the layout -- its only writer was the
// legacy openvr proxy's launch_centre.cpp, deleted with that proxy, and
// nothing native replaced it.
// _v31 because the runtime's hidden-area-mesh triangle counts joined
// (runtimeMaskTri), for fix.eye_mask's auto mode (frame_flag.h).
// _v30 because the detector's verdict on a jump crosses to the openvr half
// (jumpVerdict), which waits for it before restarting the history.
// _v29 because the head lock's two angles are packed with a bias that fits
// the field they are masked into; the old pair would read each other's
// angles 409.6 degrees out.
// _v28 because the frame timing sample names its layout (184 or 176) and
// the WaitGetPoses block time crosses for the render thread's busy time.
// _v27 because the frame timing sample is now the SETTLED record (two
// compositor frames back) and carries the compositor's CPU time and the
// poses-ready and frame-ready stamps.
// _v26 because the frame timing sample gained the app's busy time (fpsVR's
// CPU frametime), and the EDVR-activity and head-lock words.
// _v25 because the compositor's frame timing joined, for the menu's
// Monitor page (frame_flag.h, docs/settings-menu.md).
// _v24 because the settings menu's channel joined: the anchor, the
// visibility heartbeat, the drawn counter and the runtime kind
// (frame_flag.h, docs/settings-menu.md).
// _v23 because the eye-tracked gaze joined, for the foveation's moving
// centre (frame_flag.h).
// _v22 because the scene-arrived latch joined, so the cull guard can hold
// off while the intro is up (frame_flag.h).
// _v21 because
// on an assumption of symmetry that a Quest 3 breaks (frame_flag.h).
// _v20 because the game's D3D11 device joined, for the early VR handover
// (since removed; the cull guard's presence test keeps the field).
// _v19 because the head pose joined, for the intro movie's world-space
// panel (intro_panel.h). _v18 because the arrival stamp joined for the
// window-scoped heal. _v17
// added the centring servo's redo counter.
// _v16 carried both eyes' corner sets for the nose-mask stitch. _v15 was one eye's corners, _v14 the 4-float rect,
// _v13 fssBodyStamp, _v12 fssChromeStamp, _v11 fssMonoFrames, _v10
// submitTex. _v9 was
// headForward. _v8 was
// eyeTangents, _v7 cullGuard, _v6 eyeSize, _v5 holdFrames, _v4
// externalCamStamp, _v3 the field before that. A mismatched pair from
// different builds must not agree on a layout they disagree about, and a
// d3d11.dll writing a ninth field into an eight-field mapping made by an
// older openvr_api.dll would write past the end of it.
//
// The version bump matters more for these later fields than for the early ones.
// An old openvr_api.dll paired with a new d3d11.dll would find no stamp at all,
// read zeros, and conclude the gate is dead -- which fails safe -- but the
// reverse pairing would have a new reader trusting a stamp nobody writes.
// Separate mappings make both pairings inert instead of subtly wrong.
//
// eyeSize and cullGuard are built to survive that pairing on their own as
// well: an unmatched reader sees 0, which every caller is required to read as
// "no answer" and fall back on. Mismatched halves therefore behave exactly
// like a session with no openvr proxy installed, which is a supported
// configuration and not a fault.
const wchar_t* mappingName() {
    static wchar_t name[64];
    static bool built = false;
    if (!built) {
        _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_glitch_frame_v35_%lu",
                     GetCurrentProcessId());
        built = true;
    }
    return name;
}

Shared* block() {
    static Shared* s = [] () -> Shared* {
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(Shared), mappingName());
        if (!h) return nullptr;
        // Deliberately not closed. The mapping must outlive both proxies, and a
        // handle leaked once per process is the cheapest way to guarantee it.
        void* p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
        return static_cast<Shared*>(p);
    }();
    return s;
}

// THE ROLL-CALL (since v34; frame_flag.h, "The layout version"). Its name
// carries no version, so it outlives every layout change. Each half signs
// it once, at its first use of the channel: `first` takes the version of
// whichever half came first, a later half on a different version writes
// its own into `other`, and `signers` counts the signatures. Both slots
// are read on every call, so the half that came first refuses the moment a
// mismatched partner signs; neither slot is ever cleared. These three words
// are fixed for good: a mapping keeps the size its creator gave it, so a
// later build that grew the struct could not map an older half's roll-call
// and would see no partner at all.
struct RollCall {
    volatile LONG first;
    volatile LONG other;
    volatile LONG signers;
};

// The last layout that predates the roll-call (v0.17.0 shipped it). A half
// built with it never signs, so its block is looked for by name instead.
constexpr uint32_t kUnsignedLayout = 33;

RollCall* rollCall() {
    static RollCall* r = [] () -> RollCall* {
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_frame_flag_rollcall_%lu",
                     GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      sizeof(RollCall), name);
        if (!h) return nullptr;
        // Not closed, for the block's reason above.
        auto* p = static_cast<RollCall*>(
            MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(RollCall)));
        if (!p) return nullptr;
        const LONG ours = static_cast<LONG>(kFrameFlagVersion);
        const LONG was = InterlockedCompareExchange(&p->first, ours, 0);
        if (was != 0 && was != ours) InterlockedCompareExchange(&p->other, ours, 0);
        InterlockedIncrement(&p->signers);
        return p;
    }();
    return r;
}

// An unsigned (v33) partner, once found. Latched, because finding it takes
// a kernel call and it cannot leave a process it has loaded into.
volatile LONG g_unsignedPeer = 0;

// The partner's version when it differs from ours, else 0.
LONG peerMismatch() {
    if (const LONG unsignedPeer = g_unsignedPeer) return unsignedPeer;
    const RollCall* r = rollCall();
    if (!r) return 0;
    const LONG ours = static_cast<LONG>(kFrameFlagVersion);
    const LONG first = r->first, other = r->other;
    if (first && first != ours) return first;
    if (other && other != ours) return other;
    return 0;
}

// Every accessor comes through here: a half whose partner runs another
// layout reads "no answer" and writes nothing.
Shared* map() {
    if (peerMismatch()) return nullptr;
    return block();
}

}  // namespace

uint32_t frameFlagPeerMismatch() {
    if (const LONG theirs = peerMismatch()) return static_cast<uint32_t>(theirs);
    // A second signature is a partner on our own layout: nothing to look for.
    const RollCall* r = rollCall();
    if (r && r->signers >= 2) return 0;
    wchar_t name[64];
    _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_glitch_frame_v%u_%lu", kUnsignedLayout,
                 GetCurrentProcessId());
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!h) return 0;
    CloseHandle(h);
    InterlockedCompareExchange(&g_unsignedPeer, static_cast<LONG>(kUnsignedLayout), 0);
    return kUnsignedLayout;
}

namespace {
volatile LONG g_worldJump = 0;
}  // namespace

void noteWorldJump() { InterlockedExchange(&g_worldJump, 1); }

bool takeWorldJump() { return InterlockedExchange(&g_worldJump, 0) != 0; }

void publishHeadPose(const float* m12) {
    Shared* s = map();
    if (!s || !m12) return;
    for (int i = 0; i < 12; ++i) s->headPoseM[i] = m12[i];
    InterlockedIncrement(&s->headPoseSeq);
}

bool headPose(float* out12) {
    Shared* s = map();
    if (!s || !out12 || s->headPoseSeq == 0) return false;
    for (int i = 0; i < 12; ++i) out12[i] = s->headPoseM[i];
    return true;
}

void publishFssPanelRect(const float* corners16) {
    Shared* s = map();
    if (!s || !corners16) return;
    for (int i = 0; i < 16; ++i) s->fssPanelRect[i] = corners16[i];
    InterlockedIncrement(&s->fssPanelRectSeq);
}

void bumpFssArrivalStamp() {
    Shared* s = map();
    if (s) InterlockedIncrement(&s->fssArrivalStamp);
}

long fssArrivalStampValue() {
    Shared* s = map();
    return s ? s->fssArrivalStamp : 0;
}

bool readFssPanelRect(float* out16) {
    Shared* s = map();
    if (!s || !out16 || s->fssPanelRectSeq == 0) return false;
    for (int i = 0; i < 16; ++i) out16[i] = s->fssPanelRect[i];
    return true;
}

void bumpFssChromeStamp() {
    Shared* s = map();
    if (s) ++s->fssChromeStamp;
}

LONG fssChromeStampValue() {
    Shared* s = map();
    return s ? s->fssChromeStamp : 0;
}

void publishSubmitTexture(int eye, void* texture) {
    Shared* s = map();
    if (!s || eye < 0 || eye > 1) return;
    s->submitTex[eye] = reinterpret_cast<LONG64>(texture);
}

void* submittedTexture(int eye) {
    Shared* s = map();
    if (!s || eye < 0 || eye > 1) return nullptr;
    return reinterpret_cast<void*>(s->submitTex[eye]);
}

void publishGameDevice(void* device) {
    Shared* s = map();
    if (!s) return;
    s->gameDev = reinterpret_cast<LONG64>(device);
}

void* gameDevice() {
    Shared* s = map();
    if (!s) return nullptr;
    return reinterpret_cast<void*>(s->gameDev);
}

void announceSceneArrived() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->sceneArrived, 1);
}

bool sceneArrived() {
    Shared* s = map();
    if (!s) return false;
    return InterlockedCompareExchange(&s->sceneArrived, 0, 0) != 0;
}

void markGlitchFrame() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->flag, 1);
}

bool glitchFrameMarked() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->flag, 0, 0) != 0;
}

void unmarkGlitchFrame() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->flag, 0);
}

void clearGlitchFrame() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->flag, 0);
}

void announceGlitchConsumer() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->consumer, 1);
}

void retireGlitchConsumer() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->consumer, 0);
}

bool glitchConsumerPresent() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->consumer, 0, 0) != 0;
}

void noteJumpVerdict(uint32_t verdict) {
    Shared* s = map();
    if (!s) return;
    // The d3d11 half's own count of verdicts, above the verdict's two bits:
    // the same verdict twice running still reads as new to the openvr half.
    static uint32_t count = 0;
    ++count;
    InterlockedExchange(&s->jumpVerdict, static_cast<LONG>((count << 2) | (verdict & 3u)));
}

uint32_t jumpVerdictPacked() {
    Shared* s = map();
    return s ? static_cast<uint32_t>(InterlockedCompareExchange(&s->jumpVerdict, 0, 0)) : 0u;
}

void setExternalCameraOnFoot(bool on) {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->externalCam, on ? 1 : 0);
    // The stamp moves on every call, not on every change. A gate that has
    // settled on "no" is publishing just as actively as one that is toggling,
    // and a reader that could not tell those apart would have to treat silence
    // as consent.
    InterlockedIncrement(&s->externalCamStamp);
}

bool externalCameraOnFoot() {
    Shared* s = map();
    // FALSE when the mapping could not be made, which is the safe direction: a
    // head offset that fails to apply leaves the game exactly as it was, while
    // one that fails to STOP applying moves the player's viewpoint in the
    // cockpit. Unlike the glitch flag, this one persists across frames, so a
    // wrong answer here does not expire on its own -- which is what
    // externalCameraOnFootLive is for.
    return s && InterlockedCompareExchange(&s->externalCam, 0, 0) != 0;
}

void requestSubmitHold(uint32_t frames) {
    Shared* s = map();
    if (!s) return;
    // Set, not added. A second press during a hold restarts it rather than
    // extending it: two transitions in quick succession is one event as far as
    // the player is concerned, and adding would let a rapid double-press hold
    // for twice as long as either press asked for.
    InterlockedExchange(&s->holdFrames, static_cast<LONG>(frames));
}

void announceEyeTextureSize(uint32_t width, uint32_t height) {
    Shared* s = map();
    if (!s) return;
    // Refused rather than truncated. A size that does not fit the packing is a
    // size this was not written for, and half of it is worse than none of it:
    // the reader compares for equality, so a truncated width would answer "not
    // an eye texture" for every target including the real ones.
    if (!width || !height || width > 0xFFFFu || height > 0xFFFFu) return;
    InterlockedExchange(&s->eyeSize,
                        static_cast<LONG>((width << 16) | height));
}

bool eyeTextureSize(uint32_t* width, uint32_t* height) {
    Shared* s = map();
    if (!s) return false;
    const LONG packed = InterlockedCompareExchange(&s->eyeSize, 0, 0);
    if (!packed) return false;
    const uint32_t v = static_cast<uint32_t>(packed);
    if (width) *width = v >> 16;
    if (height) *height = v & 0xFFFFu;
    return true;
}

void announceCullGuardState(uint32_t stage, float factorH, float factorV) {
    Shared* s = map();
    if (!s) return;
    // Stage 0 clears the whole word: "off" and "no answer" are deliberately
    // the same value, because every reader must treat them identically.
    if (stage == 0) {
        InterlockedExchange(&s->cullGuard, 0);
        return;
    }
    // Clamped rather than refused, unlike eyeSize's packing check, and the
    // difference is what the field is FOR. A refused eye size would make an
    // equality test miss real targets; this is attribution, where a margin
    // saturated at +409.5% still names the right frames, while a refusal
    // would stamp a live guard as "off" -- a lie in the data the channel
    // exists to make honest.
    auto perMille = [](float factor) -> uint32_t {
        if (!(factor > 1.0f)) return 0;                    // NaN lands here too
        const float pm = (factor - 1.0f) * 1000.0f + 0.5f;
        if (pm >= 4095.0f) return 4095u;
        return static_cast<uint32_t>(pm);
    };
    const uint32_t packed = ((stage > 2 ? 2u : stage) << 24) |
                            (perMille(factorH) << 12) | perMille(factorV);
    InterlockedExchange(&s->cullGuard, static_cast<LONG>(packed));
}

uint32_t cullGuardStatePacked() {
    Shared* s = map();
    if (!s) return 0;
    return static_cast<uint32_t>(InterlockedCompareExchange(&s->cullGuard, 0, 0));
}

void announceEyeTangents(float outerMag, float innerMag) {
    Shared* s = map();
    if (!s) return;
    // Refused rather than truncated, eyeSize's rule: a tangent that does not
    // fit the packing is a value this was not written for. 65 covers a 89.1
    // degree half-angle; no headset is within a factor of ten of it.
    if (!(outerMag > 0.0f) || !(innerMag > 0.0f) || outerMag >= 65.0f ||
        innerMag > outerMag) {
        return;
    }
    const uint32_t o = static_cast<uint32_t>(outerMag * 1000.0f + 0.5f);
    const uint32_t i = static_cast<uint32_t>(innerMag * 1000.0f + 0.5f);
    InterlockedExchange(&s->eyeTangents,
                        static_cast<LONG>((o << 16) | (i & 0xFFFFu)));
}

bool eyeTangents(float* outerMag, float* innerMag) {
    Shared* s = map();
    if (!s) return false;
    const LONG packed = InterlockedCompareExchange(&s->eyeTangents, 0, 0);
    if (!packed) return false;
    const uint32_t v = static_cast<uint32_t>(packed);
    if (outerMag) *outerMag = static_cast<float>(v >> 16) / 1000.0f;
    if (innerMag) *innerMag = static_cast<float>(v & 0xFFFFu) / 1000.0f;
    return true;
}

void announceEyeTangentsVertical(float topMag, float botMag) {
    Shared* s = map();
    if (!s) return;
    // Refused rather than truncated, the rule announceEyeTangents follows:
    // a value that does not fit the packing is one this was not written
    // for. Unlike the horizontal pair there is no outer/inner ordering to
    // enforce -- top and bottom are ROLES, not magnitudes, and on the
    // Quest 3 the top is the larger of the two.
    if (!(topMag > 0.0f) || !(botMag > 0.0f) || topMag >= 65.0f ||
        botMag >= 65.0f) {
        return;
    }
    const uint32_t t = static_cast<uint32_t>(topMag * 1000.0f + 0.5f);
    const uint32_t b = static_cast<uint32_t>(botMag * 1000.0f + 0.5f);
    InterlockedExchange(&s->eyeTangentsV,
                        static_cast<LONG>((t << 16) | (b & 0xFFFFu)));
}

bool eyeTangentsVertical(float* topMag, float* botMag) {
    Shared* s = map();
    if (!s) return false;
    const LONG packed = InterlockedCompareExchange(&s->eyeTangentsV, 0, 0);
    if (!packed) return false;
    const uint32_t v = static_cast<uint32_t>(packed);
    if (topMag) *topMag = static_cast<float>(v >> 16) / 1000.0f;
    if (botMag) *botMag = static_cast<float>(v & 0xFFFFu) / 1000.0f;
    return true;
}

void announceHeadForward(float tx, float ty) {
    Shared* s = map();
    if (!s) return;
    // NaN fails every comparison, so it lands on the clamp bound rather
    // than inside the packing as garbage.
    auto biased = [](float t) -> uint32_t {
        float c = t;
        if (!(c > -3.0f)) c = -3.0f;
        if (!(c < 3.0f)) c = 3.0f;
        const int32_t milli = static_cast<int32_t>(c * 1000.0f);
        return static_cast<uint32_t>(milli + 16384) & 0x7FFFu;
    };
    const uint32_t packed = 0x80000000u | (biased(tx) << 15) | biased(ty);
    InterlockedExchange(&s->headForward, static_cast<LONG>(packed));
}

bool headForward(float* tx, float* ty) {
    Shared* s = map();
    if (!s) return false;
    const LONG packed = InterlockedCompareExchange(&s->headForward, 0, 0);
    if (!packed) return false;
    const uint32_t v = static_cast<uint32_t>(packed);
    if (tx) *tx = (static_cast<int32_t>((v >> 15) & 0x7FFFu) - 16384) / 1000.0f;
    if (ty) *ty = (static_cast<int32_t>(v & 0x7FFFu) - 16384) / 1000.0f;
    return true;
}

void announceRuntimeMaskTriangles(uint32_t leftTri, uint32_t rightTri) {
    Shared* s = map();
    if (!s) return;
    auto clamp15 = [](uint32_t n) -> uint32_t { return n > 0x7FFFu ? 0x7FFFu : n; };
    const uint32_t packed = 0x80000000u | (clamp15(leftTri) << 15) | clamp15(rightTri);
    InterlockedExchange(&s->runtimeMaskTri, static_cast<LONG>(packed));
}

bool runtimeMaskTriangles(uint32_t* leftTri, uint32_t* rightTri) {
    Shared* s = map();
    if (!s) return false;
    const LONG packed = InterlockedCompareExchange(&s->runtimeMaskTri, 0, 0);
    if (!(static_cast<uint32_t>(packed) & 0x80000000u)) return false;
    const uint32_t v = static_cast<uint32_t>(packed);
    if (leftTri) *leftTri = (v >> 15) & 0x7FFFu;
    if (rightTri) *rightTri = v & 0x7FFFu;
    return true;
}

void publishMenuAnchor(const float* m12) {
    Shared* s = map();
    if (!s || !m12) return;
    for (int i = 0; i < 12; ++i) s->menuAnchorM[i] = m12[i];
    InterlockedIncrement(&s->menuAnchorSeq);
}

bool menuAnchor(float* out12, uint32_t* seq) {
    Shared* s = map();
    if (!s) return false;
    const LONG q = InterlockedCompareExchange(&s->menuAnchorSeq, 0, 0);
    if (seq) *seq = static_cast<uint32_t>(q);
    if (q == 0) return false;
    if (out12) {
        for (int i = 0; i < 12; ++i) out12[i] = s->menuAnchorM[i];
    }
    return true;
}

void setMenuVisible(float alpha) {
    Shared* s = map();
    if (!s) return;
    float a = alpha;
    if (!(a > 0.0f)) a = 0.0f;   // NaN lands on "not visible"
    if (a > 1.0f) a = 1.0f;
    InterlockedExchange(&s->menuAlphaMille, static_cast<LONG>(a * 1000.0f + 0.5f));
    InterlockedIncrement(&s->menuVisibleStamp);
}

bool menuVisible(float* alpha, uint32_t* stamp) {
    Shared* s = map();
    if (!s) return false;
    if (stamp) *stamp = static_cast<uint32_t>(InterlockedCompareExchange(&s->menuVisibleStamp, 0, 0));
    const LONG mille = InterlockedCompareExchange(&s->menuAlphaMille, 0, 0);
    if (alpha) *alpha = static_cast<float>(mille) / 1000.0f;
    return mille > 0;
}

void bumpMenuDrawn() {
    Shared* s = map();
    if (s) InterlockedIncrement(&s->menuDrawn);
}

uint32_t menuDrawnValue() {
    Shared* s = map();
    return s ? static_cast<uint32_t>(InterlockedCompareExchange(&s->menuDrawn, 0, 0)) : 0;
}

void setMenuHeadLock(bool on, float yawDeg, float pitchDeg) {
    Shared* s = map();
    if (!s) return;
    if (!on) {
        InterlockedExchange(&s->menuHeadLock, 0);
        return;
    }
    auto tenths = [](float deg) -> uint32_t {
        float c = deg;
        if (!(c > -180.0f)) c = -180.0f;
        if (!(c < 180.0f)) c = 180.0f;
        return static_cast<uint32_t>(static_cast<int32_t>(c * 10.0f) + kHeadLockBias) & 0xFFFu;
    };
    const uint32_t packed = 0x80000000u | (tenths(yawDeg) << 12) | tenths(pitchDeg);
    InterlockedExchange(&s->menuHeadLock, static_cast<LONG>(packed));
}

bool menuHeadLock(float* yawDeg, float* pitchDeg) {
    Shared* s = map();
    if (!s) return false;
    const uint32_t v = static_cast<uint32_t>(InterlockedCompareExchange(&s->menuHeadLock, 0, 0));
    if (!(v & 0x80000000u)) return false;
    if (yawDeg) *yawDeg = (static_cast<int32_t>((v >> 12) & 0xFFFu) - kHeadLockBias) / 10.0f;
    if (pitchDeg) *pitchDeg = (static_cast<int32_t>(v & 0xFFFu) - kHeadLockBias) / 10.0f;
    return true;
}

bool takeSubmitHoldFrame() {
    Shared* s = map();
    if (!s) return false;
    const LONG n = InterlockedCompareExchange(&s->holdFrames, 0, 0);
    if (n <= 0) return false;
    InterlockedDecrement(&s->holdFrames);
    return true;
}

void requestIntroRecentre() {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->introRecentre, 1);
}

bool introRecentreRequested() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->introRecentre, 0, 0) != 0;
}

void clearIntroRecentreRequest() {
    Shared* s = map();
    if (s) InterlockedExchange(&s->introRecentre, 0);
}

void requestPoseReaderTrace(bool on) {
    Shared* s = map();
    if (!s) return;
    InterlockedExchange(&s->poseReaderRequest, on ? 1 : 0);
}

bool poseReaderTraceRequested() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->poseReaderRequest, 0, 0) != 0;
}

void publishPoseReaderCall(const PoseReaderCall& call) {
    Shared* s = map();
    if (!s) return;
    s->poseReaderRenderPtr = static_cast<LONG64>(call.renderPtr);
    s->poseReaderGamePtr = static_cast<LONG64>(call.gamePtr);
    s->poseReaderQpc = static_cast<LONG64>(call.qpc);
    s->poseReaderRenderCount = static_cast<LONG>(call.renderCount);
    s->poseReaderGameCount = static_cast<LONG>(call.gameCount);
    s->poseReaderThreadId = static_cast<LONG>(call.threadId);
    LONG flags = 0;
    if (call.renderOnStack) flags |= 1;
    if (call.gameOnStack) flags |= 2;
    if (call.wasGetLastPoses) flags |= 4;
    s->poseReaderFlags = flags;
    // Last: a reader that samples seq before touching the rest of the
    // fields and again after can tell a torn read from a fresh one.
    InterlockedIncrement(&s->poseReaderSeq);
}

PoseReaderCall poseReaderCall() {
    PoseReaderCall out{};
    Shared* s = map();
    if (!s) return out;
    out.seq = static_cast<uint32_t>(s->poseReaderSeq);
    out.renderPtr = static_cast<uint64_t>(s->poseReaderRenderPtr);
    out.gamePtr = static_cast<uint64_t>(s->poseReaderGamePtr);
    out.qpc = static_cast<uint64_t>(s->poseReaderQpc);
    out.renderCount = static_cast<uint32_t>(s->poseReaderRenderCount);
    out.gameCount = static_cast<uint32_t>(s->poseReaderGameCount);
    out.threadId = static_cast<uint32_t>(s->poseReaderThreadId);
    const LONG flags = s->poseReaderFlags;
    out.renderOnStack = (flags & 1) != 0;
    out.gameOnStack = (flags & 2) != 0;
    out.wasGetLastPoses = (flags & 4) != 0;
    return out;
}

bool externalCameraOnFootLive(uint32_t maxAgeFrames) {
    Shared* s = map();
    if (!s) return false;
    const LONG stamp = InterlockedCompareExchange(&s->externalCamStamp, 0, 0);

    // Reader-side state, so the writer needs no cooperation beyond bumping the
    // stamp. Function-local statics: each DLL has its own copy, and only the
    // openvr half calls this, once per frame from WaitGetPoses.
    static LONG lastStamp = 0;
    static uint32_t sinceMoved = 0;
    static bool everMoved = false;

    if (stamp != lastStamp) {
        lastStamp = stamp;
        sinceMoved = 0;
        everMoved = true;
    } else if (everMoved && sinceMoved < 0xFFFFFFFFu) {
        ++sinceMoved;
    }

    // Never moved means d3d11.dll has not published once -- not installed, or
    // its hooks never committed. That is not a "no" that has gone stale, it is
    // an absence of anybody to ask, and guessing "yes" would apply the offset
    // in every mode with no gate at all.
    if (!everMoved) return false;
    if (sinceMoved > maxAgeFrames) return false;
    return InterlockedCompareExchange(&s->externalCam, 0, 0) != 0;
}

}  // namespace edvr

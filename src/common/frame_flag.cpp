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
    // fssMonoFrames  frames the openvr half should submit the RIGHT eye's
    //                texture for BOTH eyes, counting down, set by d3d11
    //                when a camera jump lands while the Full System
    //                Scanner's screen is up
    //
    // The measured defect (docs/fss-scanner.md, round 33): for ~10 frames
    // of the zoom arrival the LEFT submitted image carries hard-black
    // unresolved tiles the right does not (16 vs 2 measured). The body
    // sits at optical infinity, so the right eye's image is correct for
    // both during that window; holdFrames' disciplines carry over whole.
    volatile LONG fssMonoFrames;
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
// _v11 because the struct changed again -- fssMonoFrames was added. _v10
// was submitTex. _v9 was
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
        _snwprintf_s(name, _TRUNCATE, L"Local\\edvr_glitch_frame_v11_%lu",
                     GetCurrentProcessId());
        built = true;
    }
    return name;
}

Shared* map() {
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

}  // namespace

void setFssMonoFrames(int n) {
    Shared* s = map();
    if (!s || n < 0 || n > 60) return;
    // Extend, never shorten: two jumps in quick succession keep the
    // longer window.
    if (n > s->fssMonoFrames) s->fssMonoFrames = n;
}

int fssMonoRemaining() {
    Shared* s = map();
    return s ? s->fssMonoFrames : 0;
}

void decFssMonoFrames() {
    Shared* s = map();
    if (s && s->fssMonoFrames > 0) --s->fssMonoFrames;
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

bool glitchConsumerPresent() {
    Shared* s = map();
    return s && InterlockedCompareExchange(&s->consumer, 0, 0) != 0;
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

bool takeSubmitHoldFrame() {
    Shared* s = map();
    if (!s) return false;
    const LONG n = InterlockedCompareExchange(&s->holdFrames, 0, 0);
    if (n <= 0) return false;
    InterlockedDecrement(&s->holdFrames);
    return true;
}

bool externalCameraEverPublished() {
    Shared* s = map();
    // The stamp only ever moves when setExternalCameraOnFoot is called, so a
    // nonzero stamp is proof somebody published -- regardless of what they said.
    return s && InterlockedCompareExchange(&s->externalCamStamp, 0, 0) != 0;
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

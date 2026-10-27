#pragma once
// Native on foot's avatar hooks (docs\design-native-onfoot-vr-2026-10-13.md): three read-mostly hooks on the game's avatar and camera code and the
// frame-thread half that installs them, opens their gates and holds the avatar dither-fade global for native stereo's body. avatar_hooks_core.h and
// avatar_skeleton_core.h hold the decisions and the tables; this file's .cpp holds the hooks, the census and the log.
//
// THE HOOKS (build 334925 only; any PE or prologue mismatch stands that hook down with one line and patches nothing). Installed from launch while
// fix.native_on_foot is on (native_on_foot.cpp says so through nativeOnFootHooksBody):
//   +0x2DF3EA0  the camera controller's update (vtable only): the mode byte is read after the game's update, for the camera signal (F93d).
//   +0x3DD8C60  the avatar's dither fade (rcx = the AvatarModelComponent), inside the submit job: HEAD HIDING runs in its post-call, zeroing the view masks
//               of the local third-person avatar's head parts while native stereo's body is on; the neck census, the first-person body (F15), the clip
//               modifiers (F19) and the avatar census (F3) run there too. Optional: a stand-down costs those, never the rest.
//   +0xFDEBB0  the skeleton interface's FindJoint (called by many systems): the capture of the skeletons the game attaches the local avatars to, which is
//               how the local third-person avatar is told from the others. Optional likewise.
//
// THREADS. The hooks run on whichever thread the game's job system calls the functions from. Each takes no lock it waits on, allocates nothing and
// writes no log line: it talks to the frame thread through atomics and small rings of its own.
#include <cstddef>
#include <cstdint>
#include "avatar_hooks_core.h"

namespace edvr {

// Once a frame at the Present boundary, from native on foot's own tick in device_hook.cpp (tkNativeOnFoot), BEFORE nativeOnFootFrameBoundary: so the
// want and the body nativeOnFootFrameBoundary publishes act at the next boundary, and the hooks are in place from the first frame the native key is
// on. Installs the hooks the first time they are wanted, arms the skeleton reads, logs the census and the latch, holds or gives back the avatar fade
// global, and sets the gates. Render thread. Never call it from inside a game hook.
void nativeOnFootHooksFrameBoundary();

// The skeleton interface's FindJoint(name) (EliteDangerous64.exe+0xFDEBB0, called by many systems). Native on foot hooks it (the original first, its result
// unchanged) to learn which skeleton interfaces the game attaches the local player's two avatars to: FUN 0x19B3F40 calls FindJoint("def_c_povCamera_joint")
// from two sites for EVERY humanoid (F6: the last site-1 attach is usually an NPC's). Site 1 (returning to +0x19B3FD5) is a humanoid's third-person avatar,
// site 2 its first-person avatar, which only the local player has. So the hook LATCHES a pair only when one thread makes a site-1 attach and then a site-2
// attach at the same stack location (one invocation of 0x19B3F40 does both); sites 0 and 1 below are the latched local third-person and first-person
// skeletons. Head hiding and the census use the latched pair only.
struct AvatarSkeleton {
    uint64_t iface = 0;       // the LATCHED local avatar's skeleton for this site (0 = none latched yet)
    uint32_t index = 0xFFFF;  // the povCamera joint index the original returned for it
    uint32_t captures = 0;    // RAW attaches seen at this site since launch: EVERY humanoid's, not only the local one
    uint32_t latches = 0;     // times a local pair was latched
};
AvatarSkeleton avatarSkeleton(int site);
uint64_t avatarFindJointSeen();                         // every FindJoint call the hook has seen (proof it is alive)

// An unload (FreeLibrary): puts the avatar dither-fade global back to -1 if EDVR still holds it at 0. Frame-thread context; SEH-guarded.
// device_hook.cpp calls it AFTER nativeOnFootShutdown (which puts the gate byte back) and before the hooks' storage is left behind.
void nativeOnFootHooksShutdown();

// The game image base, when its PE identity (timestamp and size) is build 334925's; false, with a sentence in `why`, otherwise. The
// answer is cached for the session. The fade global and the skeleton reads resolve their addresses from it.
bool avatarHooksBuildKnown(uintptr_t* base, char* why, size_t whyCap);

// Native on-foot F1 census (log only, read-only): the local third-person avatar's parts as the head-hiding capture latched them. `masks`
// holds the part's four view-mask words; `ok` is false when the read faulted. Returns the number of parts written; `*amc` is the latched
// avatar (0 when none is latched yet, then nothing is written).
struct LocalAvatarPart {
    const char* name = nullptr;
    bool head = false;
    bool ok = false;
    uint64_t masks[4] = {};
};
uint32_t localAvatarParts(LocalAvatarPart* out, uint32_t cap, uint64_t* amc);
// The dither-fade mode global (-1 auto, 0 off), read under SEH. False when unreadable.
bool avatarFadeModeRead(int32_t* mode);

// Native on-foot F3 census (log only, read-only): every distinct avatar model component (AMC) the avatar-fade hook saw while native stereo
// was on foot, collected for one window and taken by the frame thread. An AMC whose origin is more than 8 m from the latched local
// avatar's is left out (an NPC far away); the rest are ranked by "has EVASuit_Arms, Hands or Legs" and then by distance to the local one.
struct AvatarCensusPart {
    const char* name = nullptr;
    uint32_t idx = 0;
    uint32_t variants = 0;          // bit k: instance k is set
    uint64_t masks[4] = {};         // the view mask of each instance (raw)
};
struct AvatarCensusRow {
    uint64_t amc = 0;
    int32_t mode = -1;              // AMC creation params +0x14 (3 = third-person)
    uint32_t parts = 0;             // parts with at least one instance
    float dist = 0.0f;
    bool distOk = false;            // both origins were read
    bool evaSet = false;            // instances of EVASuit_Arms, EVASuit_Hands or EVASuit_Legs
    AvatarCensusPart part[54];
};
// Takes the window: writes up to `cap` rows, `*seen` (distinct AMCs collected, before the distance filter) and `*dropped` (AMCs that did not
// fit the eight slots). Clears the window. Returns the number of rows written.
uint32_t avatarCensusTake(AvatarCensusRow* out, uint32_t cap, uint32_t* seen, uint32_t* dropped);

// Native on-foot F2 body (frame thread; native_on_foot.cpp calls it once a frame, after nativeOnFootHooksFrameBoundary). `wanted`: the native key
// is on, so the hooks are installed from launch (the capture must be in place before the first on-foot avatar attaches) and the capture's gates
// are open. `body`: native stereo is on foot now, so the fade global goes to 0 (its own guard) and the local third-person avatar's head parts are
// zeroed. The first-person arms and legs are not touched. `reason` (a literal) says why a body ends; it is logged with the restore. The next
// nativeOnFootHooksFrameBoundary acts on both.
void nativeOnFootHooksBody(bool wanted, bool body, const char* reason);

// F15 (native on foot, TEMPORARY, fix.native_on_foot_hide_fp_body; frame thread). `want`: the key is on and native stereo is on foot. While it holds,
// the fade hook's post-call zeroes the four view masks of the first-person body parts and bracers (avh::kFpBodyPartNames) on the local first-person AMC
// (mode 2), with the head hiding's SEH method. Nothing is restored: the game rewrites the masks every frame. Alpha, the view filter and flag bytes are
// not touched. Logs the transitions once; `reason` (a literal) says why it ends.
void nativeOnFootFpBody(bool want, const char* reason);
// F19 (TEMPORARY): the body's clip modifiers. unclipWant = write them (fix.native_on_foot_body_unclip on, native on foot); probeWant = the 5 s read.
void nativeOnFootBodyClipTick(uint64_t nowMs, bool unclipWant, bool probeWant, const char* reason);
// F21 (TEMPORARY, fix.native_on_foot_body_hybrid; frame thread). `want`: the key is on and native stereo is on foot. While it holds, the hook
// (bodyClipStep, fpBodyStep) unclips the third-person torso, zeroes the third-person arms, hands and shoulders' view masks, and zeroes only the
// first-person skeleton's masks on the first-person AMC. Logs the transitions and a 5 s census line.
void nativeOnFootHybridTick(uint64_t nowMs, bool want, const char* reason);
struct FpBodyReadback {
    uint64_t amc = 0;                                 // the first-person AMC last zeroed (0 = none yet this session)
    uint32_t parts = 0;                               // how many names were found in the table
    const char* name[avh::kFpBodyPartCount] = {};       // their names, in avh::kFpBodyPartNames order
    uint64_t masks[avh::kFpBodyPartCount][4] = {};      // four view masks each, read straight after the zero
    uint64_t mode2Seen = 0;                           // first-person AMC calls while the body was wanted
};
FpBodyReadback nativeOnFootFpBodyReadback();

// Native on-foot F1 neck census (log only, read-only; native_on_foot_hooks.cpp neckCensusStep): the newest sample. `state` 5 means the head joint
// was read, in avatar-root model space; `amc` is the local AMC's origin in world space (`amcState` 1 when read). Two different spaces:
// the difference between them is not a camera offset. False until a sample exists.
struct NeckCensusReading {
    uint32_t state = 0;
    uint64_t stamp = 0;
    float head[3] = {};
    float amc[3] = {};
    int amcState = 0;
    uint32_t neckState = 0;      // F39: 1 read (neck, avatar-root model space), 2 no candidate name answered, 3 not plausible
    float neck[3] = {};
    const char* neckName = nullptr;   // F39: the candidate name the neck was found under
    uint32_t shState = 0;             // F43: bit 0 the 3P shoulders (L, R) read, bit 1 the FP shoulders read
    float sh3P[6] = {};               // F43: the 3P shoulders, avatar-root model space (L, R)
    float shFp[6] = {};               // F43: the FP shoulders, FP avatar-root model space (L, R)
    float ext3P[12] = {};             // F45: the 3P hands (L, R) and attach points (L, R), avatar-root model space
    float extFp[12] = {};             // F45: the same on the FP skeleton
    // F48: shState bit 4 (0x10): the 3P def_c_povCamera_joint read (FindJoint once per skeleton), avatar-root model space, and its index.
    float pov3P[3] = {};
    uint32_t povIndex = 0xFFFFu;
};
bool neckCensusRead(NeckCensusReading* out);
uint64_t localAmcPointer();   // F18: the local third-person AMC last matched by the fade hook (0 = none); the latch the census reads
// F93d: the camera signal's side in the avatar hooks, for native on foot's lines: whether the controller hook is armed (or stood down, and why),
// the controller last read and how often another answered, the reads that faulted, and the writes each fade-hook step held back in the third-person camera.
struct NativeOnFootCameraStats {
    int controllerHook = 0;          // 0 not tried, 1 armed, 2 stood down
    const char* why = "";            // the stand-down's reason (static storage)
    uint64_t controller = 0, switches = 0, faults = 0, headHeld = 0, clipHeld = 0;
};
NativeOnFootCameraStats nativeOnFootCameraStats();
// F95c: the commander's head kept in the shadows (native_on_foot_f95c.h). The avatar visibility job's hook (native_on_foot.cpp, every call) calls Begin before the
// game's call and End after it: for the local third-person AMC, while the head hide writes, Begin reads the job context's views and gives the head parts the
// game's shadow-only class for the call; End puts the game's classes back and leaves a ticket the fade hook's head hide takes (it then keeps the head's shadow
// views instead of zeroing). Begin also withdraws a ticket the AMC's previous call left, so a call the swap did not cover is zeroed as before. `allow` false
// (shutdown) does only that.
namespace nofoot {
namespace f95c {
struct Swap;
}  // namespace f95c
}  // namespace nofoot
bool nativeOnFootHeadShadowBegin(void* amc, void* job, nofoot::f95c::Swap* s, bool allow) noexcept;
void nativeOnFootHeadShadowEnd(nofoot::f95c::Swap* s) noexcept;
struct NativeOnFootHeadShadowViews {   // the views of the local AMC's latest covered call, by distinct flags
    uint32_t n = 0, main = 0, shadow = 0, other = 0, shared = 0;
    uint32_t distinct = 0, more = 0;  // entries used below (at most 8), views with a flags value past them
    uint32_t flags[8] = {}, count[8] = {};
};
struct NativeOnFootHeadShadowStats {   // cumulative (the frame thread logs the change)
    uint64_t calls = 0;        // visibility calls of the local third-person AMC while the head hide writes
    uint64_t swapped = 0;      // ...that ran with the head as shadow-only
    uint64_t parts = 0;        // head parts given the shadow-only class (or none, from main-only) for those calls
    uint64_t noViews = 0;      // calls whose job context could not be read (no swap: zeroed as before)
    uint64_t lost = 0;         // classes the game changed during a call (its value stood)
    uint64_t shadowPairs = 0;  // (head instance, shadow view) pairs after the call: the head drawn into a shadow pass
    uint64_t eyePairs = 0;     // (head instance, main or other view) pairs after the call, removed by the fade hook (0 expected)
    uint64_t kept = 0, keptWords = 0, removed = 0;   // fade-hook calls that kept the shadow masks, their non-zero words, words that lost a bit
    uint64_t zeroed = 0;       // fade-hook calls that zeroed (no ticket)
    uint64_t held = 0;         // fade-hook calls that wrote nothing (the game's third-person camera, F93d)
    uint32_t faults = 0;
    bool down = false;         // three faults: F95c stood down for the session (the head hide zeroes as before)
    NativeOnFootHeadShadowViews views;
};
NativeOnFootHeadShadowStats nativeOnFootHeadShadowStats();

// F142 (TEMPORARY A/B, fix.native_on_foot_body_full_outfit; native_on_foot_f142.h). The overcoat: the game gives the coat's part (EVASuit_Gear_Back) the shadow-views-only class
// in first person, so on EDVR's body the suit shows through. While the key is on, F95c's swap the other way: the same visibility call, for the local third-person AMC while
// the head hide writes, gives every part the game left in the shadow-views-only class (2) that is not a head part and not the first-person skeleton the every-view class (1)
// for the length of the call; End puts the game's class back, and a class the game changed in between stands. No mask is written. `on` is set by the frame thread, live.
namespace nofoot {
namespace f142 {
struct OutfitSwap;
struct Census;
}  // namespace f142
}  // namespace nofoot
void nativeOnFootFullOutfitSet(bool on) noexcept;
bool nativeOnFootOutfitBegin(void* amc, nofoot::f142::OutfitSwap* s, bool allow) noexcept;
void nativeOnFootOutfitEnd(nofoot::f142::OutfitSwap* s) noexcept;
struct NativeOnFootOutfitStats {   // cumulative (the frame thread logs the change)
    uint64_t calls = 0;        // visibility calls of the local third-person AMC while the key is on and the head hide writes
    uint64_t swapped = 0;      // ...that ran with at least one part promoted
    uint64_t promoted = 0;     // parts promoted, summed over the calls
    uint64_t refused = 0;      // classes the game changed between our read and our write (left alone)
    uint64_t lost = 0;         // classes the game changed during a call (its value stood)
    uint64_t part[avh::kPartCount] = {};   // promotions by part index
    uint32_t faults = 0;
    bool down = false;         // three faults: the A/B stood down for the session
};
NativeOnFootOutfitStats nativeOnFootOutfitStats();
// The outfit census (F142), taken on the fade hook's thread for the local third-person AMC (about every 100 ms, SEH-guarded) and queued when any part's instances, class, flags
// or items differ from the last queued one (and once at the first read). The frame thread takes them one at a time; false when none is waiting.
bool nativeOnFootOutfitCensusTake(nofoot::f142::Census* out) noexcept;

#ifdef EDVR_NATIVE_ON_FOOT_HOOKS_TEST
// The rig's seam (tools\native_on_foot_hooks_test): the same boundary with a sink of the rig's, synthetic functions in place of the game's,
// and the shared state to look at.
using NativeOnFootHooksSinkFn = void (*)(void* ctx, const char* line);
struct NativeOnFootHooksTestTargets {
    uintptr_t controller = 0, avatarFade = 0, findJoint = 0;
};
namespace nofoothookstest {
constexpr int kController = 0, kFade = 1, kFind = 2;   // the hooks, in the order stolenBytes, hookState and gateOpen take them
void setTargets(const NativeOnFootHooksTestTargets& targets);
void setFadeGlobal(int32_t* mode, float* amount);   // the synthetic dither-fade global and its amount float (null: the build's real addresses)
void setHeadImage(uintptr_t base, size_t size);     // the rig's synthetic game image for the skeleton reads' arming (base 0: the real module)
void boundary(NativeOnFootHooksSinkFn fn, void* ctx);   // nativeOnFootHooksFrameBoundary, its lines to the sink
void clipProbeClock(uint64_t now);                    // the existing 5 s probe's clock, zero restores the system clock
void clipProbeSink(NativeOnFootHooksSinkFn fn, void* ctx); // capture the bounded gear diagnostic's actual formatter
void shutdown();                                    // nativeOnFootHooksShutdown()
bool fadeOurs();                                    // EDVR holds the global at 0 for native stereo's body
size_t stolenBytes(int hook);
int hookState(int hook);                            // 0 not tried, 1 armed, 2 stood down
bool gateOpen(int hook);
uint64_t controllerCalls();
bool skeletonArmed();
void setSkeleton(int site, uint64_t iface, uint32_t index);   // a latched pair member set by hand
bool headHideOn();                 // the fade hook's post-call may hide (the native key on, both hooks armed, names verified)
bool headHideDown();               // stood down for the session (guarded accesses faulted)
int partNamesState();              // 0 not checked, 1 the table matches, 2 it does not
uint64_t hideCalls();              // post-calls that zeroed the masks of the local AMC
uint64_t hideZeroed();             // mask words that were non-zero and were zeroed
uint64_t amcCalls();               // fade calls seen with the gate open
uint64_t amcLocalMatches();        // ...that were the local third-person AMC
uint64_t amcLocal();               // the local AMC pointer last matched
void reset();                      // every hook uninstalled, every latch and counter cleared
}  // namespace nofoothookstest
#endif

}  // namespace edvr

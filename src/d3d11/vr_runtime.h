// Which VR back end this process is actually running on.
//
// WHY THIS EXISTS
//
// A commander installed both EDVR files perfectly and reported that the mod
// did nothing (2026-09-06, Rift S, Steam install, EDVR chained behind EDHM).
// Both DLLs hashed byte-for-byte against edvr_install_state.ini; the proxy
// exported all fourteen of the original's exports plus our two selftests; the
// installer's choice of Openvr\win64 was right. The install was flawless. The
// game had simply never opened it -- the in-process module list carried
// LibOVRRT64_1.dll and LibOVRRTImpl64_1.dll from the Oculus runtime and no
// openvr_api.dll whatsoever.
//
// Elite ships two VR back ends, OpenVR and Oculus' native SDK, and prefers
// Oculus whenever the Meta PC runtime is driving the headset. On that path
// openvr_api.dll is never loaded, so EDVR's openvr half cannot run, whatever
// sits on disk beside the game.
//
// AND THIS HALF SAID THE OPPOSITE. Eight messages -- four in the flash
// detector, one in the head-offset gate, two in the foveation summary, one in
// the vScreen starvation notice -- read the shared-memory consumer flag, found
// it clear, and told the user "openvr_api.dll is NOT installed". One of them
// forty times in a single session. So he chased his install through three dead
// ends (a Defender quarantine, a game-file verify, a decoy Openvr folder),
// every one of them costing a round trip, because the log kept asserting a
// fault that did not exist. The install was never the problem and the log was
// never going to say so.
//
// FOUR CAUSES, FOUR DIFFERENT NEXT MOVES. A clear consumer flag means only
// that the openvr half has not announced a validated compositor hook. That
// happens when:
//
//   - no VR runtime is loaded at all, because the session is on a monitor
//     (ordinary, not a fault, and the old text called it a WARNING);
//   - the game is on Elite's native Oculus back end, where our openvr half
//     cannot be loaded by anything we do (the case above);
//   - an openvr_api.dll IS loaded but does not carry EDVR's native-runtime
//     exports, so EDVR's runtime is genuinely not installed in the folder
//     the game loads from, or something has replaced it -- a game update
//     restoring the stock file is a recorded field failure;
//   - EDVR's native runtime is loaded and is driving the session, which is
//     terminal: the only one of the four this file ever settles on.
//
// The module list separates all four, cheaply and without asking the game
// anything. Native-versus-anything-else is settled by the
// edvrConfigureNativeRuntime/edvrGetNativeRuntimeStatus export pair rather
// than by path, because a path proves where a file sits and not what is in
// it -- and the whole failure above was a correct path.
#pragma once

namespace edvr {

enum class VrRuntime {
    NoneLoaded,     // no VR runtime library in this process
    NativeOpenXR,   // EDVR's native OpenXR openvr facade is loaded
    ForeignOpenvr,  // an openvr_api.dll is loaded, and it is not EDVR's
    OculusNative,   // no openvr_api.dll, and Oculus' LibOVR runtime is loaded
};

// The verdict. Re-measured from the process module list at most once a second,
// and never cached as final except for NativeOpenXR, because every other state
// can still turn into it seconds into a launch: on the field rig the D3D11
// device is created 1.20 s before openvr_api.dll is first called.
VrRuntime vrRuntime();

// Why the openvr half is not acting, as a CLAUSE that fits inside a sentence:
// "...nothing stopped it, because <this>." Never null, and never asserts
// anything the module list did not show.
const char* vrRuntimeShortWhy();

// The full explanation -- what was found, what it means and what to do about
// it -- on a log line OF ITS OWN, at most once a session (and once more if the
// verdict changes under it, which only a very slow launch can do).
//
// ITS OWN LINE, because the log's line buffer is 1200 bytes and the longest of
// these paragraphs is 784. Appended to the vScreen starvation notice, which is
// itself most of a buffer, it would truncate the pair mid-word -- the exact
// failure glitch_frame.cpp already carries a comment about, where a sentence
// ending in "(do" reads as a crash. Once a session, rather than per site, for
// the same reason the flash detector takes the short clause: this text was
// printed forty times in the session that made it necessary.
//
// Call it from any site that has just told the reader something is not
// happening. The first caller prints; the rest cost a flag.
void vrRuntimeExplainOnce();

// One "vr runtime:" line, said once, from the frame boundary. Held back for
// ten seconds so a launch that has not reached its runtime yet is not reported
// as having none; said immediately once the verdict is NativeOpenXR, since
// that one is terminal and worth having early. If the verdict improves after
// the line was said, exactly one correction follows it.
void vrRuntimeTick();

}  // namespace edvr

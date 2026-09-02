// Shared plumbing for the two proxy DLLs.
//
// Both edvr's d3d11.dll and openvr_api.dll work the same way: load the real
// module, resolve every export into a pointer array that generated assembly
// thunks jump through, and wrap only the handful of entry points we care about.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace edvr {

// Resolution order:
//   1. configuredPath, if non-empty. Relative paths resolve against moduleDir.
//   2. systemFallback, if non-null: a filename under %SystemRoot%\System32.
// Returns null on failure, having written a diagnostic to logDir.
HMODULE loadRealModule(const std::wstring& moduleDir, const std::string& configuredPath,
                       const wchar_t* systemFallback, const wchar_t* what);

// Reads one dotted key (e.g. "d3d11.real_dll") from <moduleDir>\<iniName>
// before Config exists to read it. Loader-lock safe: raw CreateFile/ReadFile
// into a fixed stack buffer, no dynamic growth, no map of every key -- just
// the one value a proxy needs before it can pick which real DLL to load.
// Mirrors Config::parse's section-qualified "[s]\nk = v" and bare "s.k = v"
// flattening, so a value read here and the same value read later through
// Config agree. Returns empty if the file, section+key, or value is absent.
std::string readConfigStringEarly(const std::wstring& moduleDir, const wchar_t* iniName,
                                  const char* dottedKey);

// Fills procs[i] with GetProcAddress(real, names[i]), substituting
// unresolvedStub for anything missing so a stale export list degrades to a
// no-op return rather than a jump through null.
size_t resolveProcs(HMODULE real, const char* const* names, size_t count, void** procs,
                    void* unresolvedStub);

// As above, but anything `preferred` does not export is taken from `fallback`
// before giving up on the stub. Returns the count still unresolved, and reports
// how many came from the fallback.
//
// For chaining through another proxy. EDHM and ReShade export the handful of
// entry points they care about, not all of d3d11's. Without a fallback the rest
// would become no-op stubs and the game would lose functions that work perfectly
// well in the system copy.
size_t resolveProcsChained(HMODULE preferred, HMODULE fallback, const char* const* names,
                           size_t count, void** procs, void* unresolvedStub,
                           size_t* fromFallback);

// UTF-8 to wide, for config values naming a path.
std::wstring widenUtf8(const std::string& s);

// Last-resort diagnostic for failures that happen before the log exists, or
// that stop the game from starting at all.
void writeFatalNote(const std::wstring& dir, const wchar_t* text);

// Appends one line to <exe dir>\edvr_breadcrumbs.txt.
//
// For diagnosing a crash inside DllMain, where the event log tells you the
// process died but nothing tells you how far our code got. Uses only
// CreateFile/WriteFile: no heap, no CRT formatting, no locks, nothing that can
// itself fail under loader lock. Safe to call before anything is initialised.
void breadcrumb(const char* stage);

// THE TRAIL STOPPED WHERE THE INTERESTING PART STARTS.
//
// Every crumb above is a loader-phase crumb. The last one a healthy session
// writes is "vr: submit thread", about thirteen seconds in, and the next is
// "gfx: process exit" however many hours later -- so a session that dies in
// between leaves a trail identical to one that died the instant it finished
// starting. Issue #19 is three such sessions: the logs end mid-heartbeat at 90
// fps with no fault line, no FEATURE-DISABLED and no FATAL, and the breadcrumbs
// place the death nowhere at all. A census over that reporter's whole file put
// a third of their VR sessions in this state, which is a third of the evidence
// arriving unreadable.
//
// The two calls below are what a crash needs to land somewhere.

// One line every log.breadcrumb_heartbeat_seconds (30 by default, 0 to turn it
// off), from the frame boundary. Costs an open/append/close at that cadence and
// a clock read on the frames between.
//
// What it buys is the shape of the death, within a bound. A bare "no exit
// crumb" says only that the process did not leave cleanly, at any point in a
// three-hour session. Two consecutive heartbeats carry a frame count each, so
// the trail dates the death to a half-minute window AND says whether the frames
// were still arriving at rate -- which separates a crash from the slow starve,
// and says it without needing the log at all, because breadcrumbs survive when
// a log buffer does not.
//
// What it does NOT separate is a crash from a HANG: both stop the heartbeat and
// the log at the same instant and look identical here. Read the eventual exit
// -- or its absence -- for that.
//
// Interval is log.breadcrumb_heartbeat_seconds; 0 turns it off.
void breadcrumbHeartbeat(uint64_t frameNo);

// Names the exception, the faulting address and the module it lands in, on the
// way out.
//
// This is the answer to the question issue #19 had to be sent to Event Viewer
// for. The faulting module is the whole question in a three-mod D3D chain --
// EDVR, EDHM and ReShade are all in this process, and "the game crashed" does
// not distinguish them -- and a reporter should not have to know what Event
// Viewer is to answer it.
//
// Chains to whatever filter was already installed, so the game's own crash
// reporter still runs. Installed once, on the first export call.
//
// BEST EFFORT, AND THE LIMITS ARE WORTH KNOWING BEFORE TRUSTING A BLANK TRAIL.
// SetUnhandledExceptionFilter is last-installed-wins, so anything that installs
// after us -- the game, EDHM, ReShade -- silently takes the top of the chain and
// this never runs. And EDVR's own faults on the frame path largely do not reach
// it: guard.h wraps the hook bodies in __try and guardFilter absorbs them by
// design, which is the whole point of that machinery. Not every line is inside
// one -- hookedPresent's head, the thunks, DllMain and the log flusher thread
// are not -- so what lands here is a fault nobody claimed: the game's, another
// mod's, or one of ours from outside a guard.
// A crash with no line from this is therefore evidence of very little; a crash
// WITH one names the module, which is the question.
void breadcrumbInstallCrashHandler();

// Undoes the above. Must run on the FreeLibrary path: an unloaded module that
// leaves its filter installed points the OS at freed address space, and the
// next unhandled exception in the process dies with no report from anyone.
// Only removes ours -- if another module installed on top, the chain is left
// alone rather than having that one thrown away too.
void breadcrumbRemoveCrashHandler();

// Reads the host executable's FileVersion resource, e.g. "330683".
//
// Everything edvr asserts about this game was established against one build.
// Shader hashes in particular identify one compiled shader and will not survive
// an update. Features that depend on them self-disable when their own checks
// fail, but the log should say plainly which build is running, so a report from
// someone on a different one is interpretable.
std::string gameBuildVersion();

// True if the running build is one the shader-hash-dependent features have
// actually been verified against.
bool gameBuildIsVerified();

// The builds gameBuildIsVerified() accepts, as "330683" or "330683, 332753",
// for log lines that want to name them.
//
// This is read from the list rather than written out by hand at each call
// site. A literal in a log string is right until the day someone adds a build
// and misses one, and then every log in the wild carries a number that is no
// longer the whole truth -- which is the one thing these lines exist to say.
std::string verifiedBuildList();

// Drops a breadcrumb the first time this line is reached and never again.
// For marking progress through a per-frame path without writing 90 lines a
// second: what matters is whether we got there at all, once.
#define EDVR_BREADCRUMB_ONCE(text)                    \
    do {                                              \
        static volatile long edvr_bc_done_ = 0;       \
        if (InterlockedExchange(&edvr_bc_done_, 1) == 0) ::edvr::breadcrumb(text); \
    } while (0)

}  // namespace edvr

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

// Drops a breadcrumb the first time this line is reached and never again.
// For marking progress through a per-frame path without writing 90 lines a
// second: what matters is whether we got there at all, once.
#define EDVR_BREADCRUMB_ONCE(text)                    \
    do {                                              \
        static volatile long edvr_bc_done_ = 0;       \
        if (InterlockedExchange(&edvr_bc_done_, 1) == 0) ::edvr::breadcrumb(text); \
    } while (0)

}  // namespace edvr

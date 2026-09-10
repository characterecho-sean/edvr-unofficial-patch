#include "vr_runtime.h"

#include <windows.h>
#include <psapi.h>   // EnumProcessModules: the same census foveation's ARMED line uses

#include <cstdio>
#include <cstring>

#include "../common/log.h"

namespace edvr {
namespace {

// The verdict is re-measured at most this often. Every consumer is a
// once-a-session log line or a twenty-second summary, so this could be far
// slower; it is a second because the only cost of being wrong here is a wrong
// sentence in the log, and a second is short enough that nothing reads a stale
// one.
constexpr uint64_t kRemeasureMs = 1000;

// How long to wait before saying the runtime out loud. On the field rig the
// D3D11 device is created 1.20 s before openvr_api.dll is first called, and a
// cold launch behind a launcher can be slower again -- announcing at the first
// frame would report NONE on a healthy SteamVR session every time. Ten seconds
// is past every load this project has measured and still early enough to be
// near the top of the log where a reader looks.
constexpr uint64_t kAnnounceMs = 10000;

// Our openvr_api.dll is told from anyone else's by an export, not by a path.
// The whole failure this file exists for was a CORRECT path: the file was
// exactly where it belonged and the game had loaded a different back end
// entirely. A path answers "what is on disk", and the question here is "what is
// in this process".
//
// This export has been on the openvr half since it was written, so it
// identifies old EDVR builds as ours too -- which is right. "Ours but silent"
// is a different diagnosis from "not ours", and version skew belongs in the
// first of those, not the second.
constexpr char kOursExport[] = "edvr_selftest_system_hook";

struct State {
    CRITICAL_SECTION lock{};
    bool             lockReady = false;

    VrRuntime verdict = VrRuntime::NoneLoaded;
    uint64_t  measuredMs = 0;   // 0 = never measured
    bool      settled = false;  // EdvrOpenvr, which cannot turn into anything else

    // Evidence, so every sentence below quotes something that was actually
    // seen rather than asserting a state of the world.
    char foreignPath[MAX_PATH] = {};   // the openvr_api.dll that is not ours
    char oculusModule[64] = {};        // LibOVRRT64_1.dll, or whichever matched
    bool oculusAlsoLoaded = false;     // both an openvr_api.dll AND LibOVR

    // Built once per distinct verdict rather than per call, so a reader that
    // is not holding the lock cannot see a half-written sentence. In practice
    // the verdict stops changing a few seconds into a launch and this is
    // immutable for the rest of the session.
    char whyBuf[820] = {};
    bool whyBuilt = false;

    bool announced = false;
    bool corrected = false;
    VrRuntime announcedAs = VrRuntime::NoneLoaded;
    uint64_t  firstTickMs = 0;

    uint32_t  explained = 0;
    VrRuntime explainedAs = VrRuntime::NoneLoaded;
};

// Two: the first answer, and one correction if the verdict moved under it.
constexpr uint32_t kMaxExplains = 2;

State& state() {
    static State s;
    if (!s.lockReady) {
        // Racing initialisers would both InitializeCriticalSection the same
        // object, which is benign here only because the first call happens on
        // the render thread long before anything else asks. Belt: the flag is
        // set last.
        InitializeCriticalSection(&s.lock);
        s.lockReady = true;
    }
    return s;
}

bool nameIs(const char* name, const char* want) {
    return _stricmp(name, want) == 0;
}

// LibOVRRT64_1.dll, LibOVRRTImpl64_1.dll, and the 32-bit spellings of both.
// The prefix is the stable part across Oculus runtime versions; the digits
// after it are the ABI and the bitness and both have moved before.
bool isOculusRuntimeModule(const char* name) {
    return _strnicmp(name, "LibOVRRT", 8) == 0;
}

void buildWhy(State& s) {
    switch (s.verdict) {
        case VrRuntime::OculusNative:
            snprintf(s.whyBuf, sizeof(s.whyBuf),
                     "This session is on Elite's NATIVE OCULUS back end -- %s is loaded in this "
                     "process and no openvr_api.dll is loaded at all. Elite ships two VR back ends "
                     "and prefers Oculus whenever the Meta PC runtime is driving the headset, and on "
                     "that path it never opens openvr_api.dll, so EDVR's openvr half cannot run "
                     "here whatever is installed beside the game. THIS IS NOT AN INSTALL FAULT and "
                     "reinstalling will not change it: to get these fixes you have to reach the game "
                     "through OpenVR instead -- Steam Link, or Virtual Desktop in SteamVR mode or "
                     "with OpenComposite. See \"Headsets and VR runtimes\" in the README.",
                     s.oculusModule[0] ? s.oculusModule : "an Oculus runtime module");
            break;
        case VrRuntime::ForeignOpenvr:
            snprintf(s.whyBuf, sizeof(s.whyBuf),
                     "The openvr_api.dll this process loaded is NOT EDVR's -- it is %s, which does "
                     "not export %s. So EDVR's openvr half is genuinely not installed in the folder "
                     "the game loads from, or something has replaced it. Install it from the openvr "
                     "folder in the download: rename the game's own openvr_api.dll to "
                     "openvr_api_orig.dll and put EDVR's in its place, in whichever Openvr folder "
                     "holds the file. The installer does this for you.",
                     s.foreignPath[0] ? s.foreignPath : "another copy", kOursExport);
            break;
        case VrRuntime::EdvrOpenvr:
            snprintf(s.whyBuf, sizeof(s.whyBuf),
                     "EDVR's own openvr_api.dll IS loaded here, and has not announced a validated "
                     "compositor hook -- so this is not a missing file. Read edvr_vr_*.log beside "
                     "this one: it will say whether the compositor version was one it does not know, "
                     "whether the hook was refused, or whether it is a build older than this "
                     "d3d11.dll (mismatched halves share no state by design, and behave exactly like "
                     "a session with no openvr proxy). %s",
                     s.oculusAlsoLoaded
                         ? "An Oculus runtime module is loaded in this process as well, so it is "
                           "also possible the game opened openvr_api.dll and then chose its Oculus "
                           "back end to render through."
                         : "If there is no vr log at all, the file is loaded but its startup did not "
                           "reach the point of writing one -- report that log.");
            break;
        case VrRuntime::NoneLoaded:
        default:
            snprintf(s.whyBuf, sizeof(s.whyBuf),
                     "No VR runtime library is loaded in this process at all -- neither an "
                     "openvr_api.dll nor an Oculus one. Either this session is not in VR (which is "
                     "ordinary, and nothing here is wrong), or the runtime has not started yet.");
            break;
    }
    s.whyBuilt = true;
}

// The module list, once. Cheap enough at a second's interval that nothing here
// is worth caching harder: about five hundred handles and a base name each.
void measureLocked(State& s) {
    const VrRuntime was = s.verdict;
    const bool hadForeign = s.foreignPath[0] != 0;

    HMODULE mods[768];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        // Nothing readable: leave the last verdict standing rather than
        // claiming NONE, which would be an assertion from a failed read.
        return;
    }
    size_t n = needed / sizeof(HMODULE);
    if (n > sizeof(mods) / sizeof(mods[0])) n = sizeof(mods) / sizeof(mods[0]);

    HMODULE openvrOurs = nullptr;
    HMODULE openvrForeign = nullptr;
    char    oculus[64] = {};

    for (size_t i = 0; i < n; ++i) {
        char name[MAX_PATH] = {};
        if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name))) continue;
        if (nameIs(name, "openvr_api.dll")) {
            // Two modules CAN share a base name if each was loaded by full
            // path from a different folder. Ours wins the verdict if it is
            // there at all, because ours is the one that would be acting.
            if (GetProcAddress(mods[i], kOursExport)) {
                openvrOurs = mods[i];
            } else if (!openvrForeign) {
                openvrForeign = mods[i];
            }
        } else if (!oculus[0] && isOculusRuntimeModule(name)) {
            snprintf(oculus, sizeof(oculus), "%s", name);
        }
    }

    s.oculusAlsoLoaded = oculus[0] != 0;
    snprintf(s.oculusModule, sizeof(s.oculusModule), "%s", oculus);

    if (openvrOurs) {
        s.verdict = VrRuntime::EdvrOpenvr;
        s.settled = true;
        s.foreignPath[0] = 0;
    } else if (openvrForeign) {
        s.verdict = VrRuntime::ForeignOpenvr;
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(openvrForeign, path, sizeof(path))) {
            snprintf(s.foreignPath, sizeof(s.foreignPath), "%s", path);
        } else {
            snprintf(s.foreignPath, sizeof(s.foreignPath), "%s", "a copy whose path could not be read");
        }
    } else if (oculus[0]) {
        s.verdict = VrRuntime::OculusNative;
        s.foreignPath[0] = 0;
    } else {
        s.verdict = VrRuntime::NoneLoaded;
        s.foreignPath[0] = 0;
    }

    if (s.verdict != was || (s.foreignPath[0] != 0) != hadForeign || !s.whyBuilt) {
        buildWhy(s);
    }
}

void refresh(State& s) {
    const uint64_t now = GetTickCount64();
    if (s.settled && s.measuredMs != 0) return;
    if (s.measuredMs != 0 && now - s.measuredMs < kRemeasureMs) return;
    s.measuredMs = now;
    measureLocked(s);
}

const char* shortWhyOf(VrRuntime v) {
    switch (v) {
        case VrRuntime::EdvrOpenvr:
            return "EDVR's openvr half is loaded but has not validated its compositor hook";
        case VrRuntime::ForeignOpenvr:
            return "the openvr_api.dll loaded here is not EDVR's";
        case VrRuntime::OculusNative:
            return "this session is on Elite's native Oculus back end, which never loads "
                   "openvr_api.dll";
        case VrRuntime::NoneLoaded:
        default:
            return "no VR runtime is loaded in this process";
    }
}

const char* nameOf(VrRuntime v) {
    switch (v) {
        case VrRuntime::EdvrOpenvr:    return "OpenVR, through EDVR's own openvr_api.dll";
        case VrRuntime::ForeignOpenvr: return "OpenVR, through an openvr_api.dll that is not EDVR's";
        case VrRuntime::OculusNative:  return "Elite's native Oculus back end";
        case VrRuntime::NoneLoaded:
        default:                       return "none loaded";
    }
}

}  // namespace

VrRuntime vrRuntime() {
    State& s = state();
    EnterCriticalSection(&s.lock);
    refresh(s);
    const VrRuntime v = s.verdict;
    LeaveCriticalSection(&s.lock);
    return v;
}

void vrRuntimeExplainOnce() {
    State& s = state();
    char      say[sizeof(s.whyBuf)];
    VrRuntime v;

    EnterCriticalSection(&s.lock);
    refresh(s);
    if (!s.whyBuilt) buildWhy(s);
    v = s.verdict;
    // Once, and once more only if the verdict actually moved under it -- a
    // launch slow enough to be asked before its runtime arrived. Capped, so a
    // module list that flapped could never paper the log with this.
    const bool speak = s.explained < kMaxExplains && (s.explained == 0 || v != s.explainedAs);
    if (speak) {
        ++s.explained;
        s.explainedAs = v;
        memcpy(say, s.whyBuf, sizeof(say));
    }
    LeaveCriticalSection(&s.lock);

    // Outside the lock: Log::note takes its own, and holding two in an order
    // nothing else guarantees is how a frame boundary deadlocks.
    if (speak) Log::get().note("vr runtime: %s", say);
}

const char* vrRuntimeShortWhy() {
    return shortWhyOf(vrRuntime());
}

const char* vrRuntimeName() {
    return nameOf(vrRuntime());
}

void vrRuntimeTick() {
    State& s = state();
    const uint64_t now = GetTickCount64();

    EnterCriticalSection(&s.lock);
    if (s.firstTickMs == 0) s.firstTickMs = now;
    refresh(s);
    const VrRuntime v = s.verdict;
    const bool due = (v == VrRuntime::EdvrOpenvr) || (now - s.firstTickMs >= kAnnounceMs);
    const bool speak = !s.announced && due;
    // The one correction: a launch slow enough to be announced as something
    // else before its runtime arrived. Bounded at one line, and only ever
    // towards the terminal verdict -- this must not become a thing that
    // narrates a flapping module list.
    const bool correct = s.announced && !s.corrected && v == VrRuntime::EdvrOpenvr &&
                         s.announcedAs != VrRuntime::EdvrOpenvr;
    char oculus[64];
    snprintf(oculus, sizeof(oculus), "%s", s.oculusModule);
    const bool both = s.oculusAlsoLoaded && v == VrRuntime::EdvrOpenvr;
    if (speak) {
        s.announced = true;
        s.announcedAs = v;
    }
    if (correct) s.corrected = true;
    LeaveCriticalSection(&s.lock);

    // Logging outside the lock: Log::note takes its own, and holding two locks
    // in an order nothing else guarantees is how a frame boundary deadlocks.
    if (speak) {
        Log::get().note(
            "vr runtime: %s. Read from this process's own module list, not from the files beside "
            "the game -- which is the difference between what is installed and what the game "
            "opened.%s%s",
            nameOf(v),
            v == VrRuntime::OculusNative
                ? " EDVR's openvr half cannot run on this back end, so the fixes that live in it "
                  "are inert this session; see \"Headsets and VR runtimes\" in the README."
                : "",
            both ? " (An Oculus runtime module is loaded as well.)" : "");
    }
    if (correct) {
        Log::get().note(
            "vr runtime: correction -- EDVR's own openvr_api.dll is loaded after all. The line "
            "above was said before this runtime finished starting. Said once.");
    }
}

}  // namespace edvr

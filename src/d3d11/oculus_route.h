// Executable-local bypass for Elite's legacy LibOVR selection.
//
// The route is deliberately a very small door: only a validated KERNEL32
// LoadLibraryW IAT slot in Elite's executable is exchanged.  The wrapper
// refuses one exact basename at one exact validated return address.  Runtime
// modules, all other callers, and all other names continue to the target that
// occupied the slot before EDVR.
#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>

namespace edvr {

// Shared with src/common/elite_oculus_profile.h.  The profile validator owns
// all PE parsing and executable-revision checks; this module only consumes its
// already validated result.
struct OculusProfileMatch;

// Called from DllMain before normal game startup.  It performs no allocation,
// file I/O, logging, waits, thread creation, or LoadLibrary call.  The
// nativeBuild argument is an immutable capability of the binary being loaded,
// rather than a user setting.
void oculusRouteInstallEarly(bool nativeBuild);

// Called after log initialisation and outside the loader lock.  Emits only a
// bounded number of changed counter snapshots; querying status itself is
// side-effect free.
void oculusRouteReport();

// Restores the executable slot only while it still contains our wrapper.
// Safe for an uninstalled route.  The caller must provide its normal process
// teardown quiescence before unloading this DLL.
void oculusRouteUninstallEarly();

// Versioned, fixed-storage status returned by value.  No pointers in this
// structure refer to route-owned mutable storage.
struct OculusRouteStatus {
    uint32_t version;
    uint32_t nativeBuild;
    uint32_t profileKnown;
    uint32_t slotValidated;
    uint32_t callerValidated;
    uint32_t installed;
    uint32_t originalPublished;
    uint32_t uninstallOwned;

    uint32_t installAttempts;
    uint32_t installFailures;
    uint32_t calls;
    uint32_t exactName;
    uint32_t rejected;
    uint32_t forwarded;
    uint32_t unknownCaller;
    uint32_t nearName;
    uint32_t malformedName;
    uint32_t noOriginal;
    uint32_t publicationRaces;
    uint32_t capabilityMismatches;
    uint32_t reportReady;
    uint32_t callsBeforeReportReady;
    uint32_t callsAfterReportReady;

    uint32_t lastDecision;
    uint32_t lastError;
    uint32_t reportCount;
    uint32_t reportLimitReached;

    uintptr_t iatSlot;
    uintptr_t originalTarget;
    uintptr_t executableBase;
    uintptr_t callerReturnRva;
    uintptr_t lastCallerReturnRva;
};

OculusRouteStatus oculusRouteStatusSnapshot();

// Test-only seams.  They exercise the same wrapper dispatch body while
// injecting a known original target and profile identity, so tests never
// patch a real executable or load a headset runtime.
#if defined(EDVR_OCULUS_ROUTE_TEST)
using OculusRouteLoadLibraryW = HMODULE(WINAPI*)(LPCWSTR);

void oculusRouteTestReset();
void oculusRouteTestSetProfile(void** slot, uintptr_t callerReturnRva, bool known);
using OculusRouteTestAfterCas = void(*)();
void oculusRouteTestSetAfterCas(OculusRouteTestAfterCas callback);
using OculusRouteTestBeforeCas = void(*)();
void oculusRouteTestSetBeforeCas(OculusRouteTestBeforeCas callback);
using OculusRouteTestReportSink = void(*)(const OculusRouteStatus*);
void oculusRouteTestSetReportSink(OculusRouteTestReportSink sink);
bool oculusRouteTestPublish(OculusRouteLoadLibraryW original,
                            uintptr_t executableBase,
                            uintptr_t callerReturnRva);
bool oculusRouteTestPublishSlot(void** slot, OculusRouteLoadLibraryW original,
                                uintptr_t executableBase,
                                uintptr_t callerReturnRva);
HMODULE oculusRouteTestDispatch(LPCWSTR path, uintptr_t callerReturnRva);
#endif

}  // namespace edvr

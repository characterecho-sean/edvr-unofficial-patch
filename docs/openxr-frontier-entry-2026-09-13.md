# Native Frontier entry and launch preparation

The separate-device Pimax flight on `8388961` passed; its result is recorded in
[the capture investigation](openxr-owned-capture-2026-09-13.md#pimax-result).
This checkpoint connects that ownership mode to the legacy DLL entry surface
and prepares a reversible first native Frontier flight. The prior installed
Frontier proxies remain the `2f051db` baseline until an explicit native
installation is performed. This is experimental transport integration; the
OpenVR proxy's complete set of EDVR fixes has not been ported into the native
facades.

## Entry contract

The native DLL now has the original fourteen export names at their historical
ordinals, plus `edvrConfigureNativeRuntime` and `edvrGetNativeRuntimeStatus` at
15 and 16. `VR_IsHmdPresent` reads the currently running System facade's
published CPU connection snapshot; it is false before Init and after
retirement. `VR_IsRuntimeInstalled` reports readable regular loader and
graphics files selected by immutable configuration or valid bootstrap paths. It
does not certify the runtime manifest or physical headset. `VR_RuntimePath`
returns stable UTF-8 storage for this native module's directory when those
files exist, otherwise null. None of these probes creates a session, loads
Valve/OpenComposite, or invokes XR discovery.

The three undocumented factory exports use typed, no-argument pointer returns
and return null: `VRControlPanel`, `VRDashboardManager` and `VRTrackedCamera`.
The preserved original DLL has SHA-256
`243a818d88e6251f70cf5a8a90c47a2775cae493ff9c45241ad7832b90010d18`. Its
complete factory bodies at RVAs `1BA0`, `1BF0` and `1C40` ignore incoming
arguments and return a cached pointer, null, or a factory result for
`IVRControlPanel_002`, `IVRDashboardManager_001` and `IVRTrackedCamera_001`.
The nonexecuting dumpbin evidence is archived in
`build/openxr-native-legacy-20260913/legacy-factory-abi.txt`. This establishes
these entry signatures for that binary, not a supported object interface or
lifetime contract.

Bootstrap accepts `EDVR_OPENXR_SEPARATE_DEVICE=1` only with both explicit
drive-absolute paths. An absent flag preserves V1 behavior; empty, zero,
unknown values, failed environment reads and flag-only input are rejected.
Exact `1` selects V2 separate-device configuration. The diagnostic runner can
combine `--bootstrap --separate-device`, verifies the child environment against
both paths and the ownership flag, and strips stale case variants before
constructing child-only variables. An explicit runtime JSON replaces inherited
case variants; no user/machine environment or registry setting is changed.

## Installer and launcher

`tools/install_edvr.py --target frontier --native-openxr --dll --native-receipt
ABS` stages the built native DLL as `Openvr/win64/openvr_api.dll` together with
its paired `d3d11.dll`. Both existing DLLs and the preserved original must
exist. A new receipt journals the transaction, records source/previous hashes
and backup paths, and reaches installed state only after both destinations
verify. Restoration is `--restore-native ABS`; it refuses changed active files,
changed backups/originals, or an uncertain/running game process. The native
transaction never writes the INI, original DLL or DLSS. INI changes made by the
user after staging are accepted and preserved during restore. Partial-copy and
receipt failures exercise rollback; uncertain rollback preserves recovery
evidence.

`tools/run_openxr_frontier.py` requires a fresh `EDLaunch.exe`, explicit loader
and runtime manifest, the selected build root and a verified native receipt for
a real launch. It compares the installed pair to the chosen build, records
hashes including the live INI, checks the runtime's library path and parses
Frontier's static OpenVR imports without loading its code. Its bootstrap
graphics path is the installed proxy beside Elite: the callback provider must
be the module already loaded by the game. The launcher refuses existing Elite,
EDLaunch, SteamVR or diagnostic processes and never terminates them. It writes
a launch plan before starting EDLaunch, redirects launcher output to evidence
files, returns its PID and leaves login/Play to the normal UI. Starting the
launcher is not recorded as a successful game session.

Both installer and launcher previews write nothing, including no backups,
receipts or directories. The PE reader checks the exact native name/ordinal
surface and rejects forwarders, malformed tables, unknown OpenVR names and
ordinal imports. Frontier's current executable has five named delay imports
from `openvr_api.dll`: Init, Shutdown, GenericInterface,
IsInterfaceVersionValid and GetInitToken. Other DLL imports are excluded. This
is a static-import preflight, not proof that no dynamic lookup can occur later.

## Desktop qualification

The absolute-path full build passed as `v0.16.2-75-g983197b-dirty`, with all
478 source hashes unchanged. The real DLL passed 223 explicit-configuration
checks, 229 bootstrap checks, 223 separate-mode checks and 229
bootstrap/separate checks without creating an OpenXR session. These include
failed startup, stable error strings, legacy probes, ordinal/name identity and
teardown. The PE reader passed 22 checks and validated the built sixteen-export
surface; the launcher passed 25 checks, including a real bounded Python child's
environment inheritance, mocked launch, blocked/malformed process inventory,
stale build/receipt rejection and write-free preview. Installer tests include
corrupt backups, corrupt destination/temp bytes, second-copy and receipt
failures, retained rollback evidence and successful restore. The existing WARP
shared-capture fixture passed 261 checks; this checkpoint did not repeat
hardware rendering because it changes entry/bootstrap and deployment tooling,
not capture or renderer code.

`build/openxr-frontier-entry-20260913/qualification.json` records the frozen
source hashes and thirteen archived files. Its `build/` is the fixed source
root for the upcoming native install. Native DLL SHA-256 is
`30e675712690ad58d0836cc13aa8423305c3c4faf0b6bdab4a1aaeaf7e422a0a`; graphics
DLL SHA-256 is
`5fc76d0fb240683c9cb97f37ab7591ac94f0f849594233d374152b316ff96594`. The
sanctioned installer verified both existing Frontier DLLs against the archived
`2f051db` pair. Native install and launch previews passed without creating the
proposed receipt or flight directory. The original DLL still matched the
ABI-audit hash. The launch preview found EDLaunch and the SteamVR
server/compositor/monitor running, so a real launch must wait until the user
closes them. No native game installation or launch has occurred.

## Next flight

The first native Frontier flight should use the Pimax headset with PiOpenXR and
SteamVR closed. Verify the exact installed pair before launch, use EDLaunch's
normal Play action, and inspect startup, both eyes, head tracking, loading
transitions, cockpit/on-foot rendering and normal game exit. Record any missing
EDVR feature separately from bootstrap or transport failures. Collect the
graphics log through `tools/edvr_log.py`, the launch receipt and the
corresponding Pimax client/server interval. Require separate-device ownership,
actual stereo submissions and complete cleanup; a launcher PID or a visible
desktop window alone is not a pass. Restore the recorded previous pair after
the flight when the game is closed.

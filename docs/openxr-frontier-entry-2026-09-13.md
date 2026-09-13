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

## First native Frontier flight

On 2026-09-13 the user reported, "Looked normal except EDVR wasn't loaded." The
installed native pair still passed receipt verification after the game exited.
The graphics log `edvr_gfx_20260913_171051.log` identifies
`v0.16.2-75-g983197b-dirty`, build `6AA72273`, linked at 22:23:47 UTC. It
records active LiveCopy hooks, exposure and vScreen installation, precompiled
temporal shader warmup and other graphics work. Ruled out: the graphics DLL
failed to load or was stale, because its build identity matches and its hooks
logged activity throughout this flight.

Pimax's client log identifies `EliteDangerous64.exe`, engine EDVR and the
native host's current application label `EDVR native stereo diagnostic`, on the
Crystal Super and RTX 5090 with parallel projection enabled and 5424x5356
recommended per eye. The server connected game PID 8884 at 17:10:53.300,
briefly disconnected it during discovery, reconnected it at 17:10:53.334 and
selected it as active at 17:10:53.457. It recorded 102 rendering cadence
samples for that PID, and the final client/render disconnect was at
17:12:38.146. The game process was absent afterward. This confirms actual
native in-game rendering, not just launcher startup. It does not independently
establish the exit code or complete internal resource retirement: the native
module's stdout diagnostics were not captured through EDLaunch.

The missing menu is an integration gap. At 17:11:21.071 the graphics log
records the menu key and refuses to open because no compositor hook registered
the rendering door. At 17:11:21.972 supersample resolve likewise reports no
compositor hook. The runtime detector recognizes the old proxy by
`edvr_selftest_system_hook`; the new native DLL does not export that marker, so
it is classified as foreign OpenVR and a later message incorrectly suggests
reinstalling it. The recorded loaded path is the intended Frontier
`openvr/win64/openvr_api.dll`, whose postflight hash still matches the native
package. The fix is to connect the native backend to the existing EDVR
callbacks and identify its capabilities correctly, not to replace the native
DLL with the forwarding proxy or fake the old marker. Full menu, temporal
processing and submission-dependent feature parity remain pending.

One runtime warning remains open: `xrLocateSpace` returned
`XR_ERROR_TIME_INVALID` at 17:10:56.985. The user did not report a
corresponding visual fault. Keep this timestamp for correlation with
startup/recenter sampling once native diagnostics are durable; no timing fix is
justified by this line alone. Six preserved evidence files and `result.json`
are archived under `build/openxr-frontier-entry-20260913/flight/result/`,
including the complete graphics/client logs, bounded server interval, launcher
output and live INI. The original DLL and live INI were preserved during
installation. No restoration was performed after the user's updated testing
instruction below.

## EDVR integration boundary

Source review confirms two independent menu requirements. Present already runs
`menuTick()` in the graphics proxy, but `openMenu()` rejects the request until
`glitchConsumerPresent()` is true. The forwarding compositor announces that
consumer after validating its submit hook. Separately, the panel is composited
only when `hookedSubmit()` reaches `menuDoorTreat()` and calls
`edvrMenuPanel()`. The native `submitEye()`/capture/compose path does neither,
and it does not publish the head pose used to anchor the menu. Faking the
consumer flag would accept input without creating a visible panel.

A native integration must validate its paired graphics provider, publish a
compatible pose and eye transform, and composite the panel into the producer
eye before the shared capture copy. This flight uses separate devices: the
existing menu renderer obtains the source texture's immediate context, so it
must run on the admitted game graphics callback with a game-device texture.
Calling it with an XR-owned image or directly on the XR owner would violate the
qualified ownership model. Share pure menu transform math rather than linking
foreign compositor-hook state into the native host.

Menu restoration alone will not restore the whole submission chain. Temporal
AA, crop/resolve/sharpen order, texture bounds and geometry publication,
withhold/resubmit behavior and GPU-query accounting each need an explicit
native owner and evidence. Existing menu, compositor, capture, shared-transfer
and module fixtures provide building blocks; add one coherent native-provider
test that exercises the consumer/pose/panel path and verifies the pixels
captured for both eyes. Keep the warning about missing capability until that
capability is actually connected.

## Subsequent testing workflow

The user explicitly authorizes destructive use of the Frontier installation for
these tests. Install the appropriate DLLs there and let the user launch the
game normally; scripted EDLaunch startup, per-flight backups and automatic
restoration are not required. Continue using `tools/install_edvr.py` to install
and verify the selected build, and `tools/edvr_log.py` for evidence. Do not
extend this authorization to another installation or to deleting unrelated
data.

The current package still obtains native startup paths through its process
environment. The next implementation must support an install-local
configuration so a manually started launcher can reach the native backend
without inherited scripted variables. Preserve explicit API and valid
environment precedence; reject malformed environment rather than silently
falling through to a local file. The OpenXR manifest selection must remain
local to the game, without changing persistent user/machine environment or the
runtime registry. Keep the already installed native DLL pair in Frontier while
this work proceeds.

## Next flight checks

The first native Frontier flight should use the Pimax headset with PiOpenXR and
SteamVR closed. Verify the exact installed pair before launch, use EDLaunch's
normal Play action, and inspect startup, both eyes, head tracking, loading
transitions, cockpit/on-foot rendering and normal game exit. Record any missing
EDVR feature separately from bootstrap or transport failures. Collect the
graphics log through `tools/edvr_log.py`, the launch receipt and the
corresponding Pimax client/server interval. Require separate-device ownership,
actual stereo submissions and complete cleanup; a launcher PID or a visible
desktop window alone is not a pass. Native startup/teardown diagnostics must be
durable independently of launcher stdout before treating internal cleanup as
qualified.

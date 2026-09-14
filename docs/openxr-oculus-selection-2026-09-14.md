# Elite's legacy Oculus selection

The release goal is to migrate every EDVR user to the native OpenXR backend,
including Meta and older Oculus users whom Elite currently routes through
LibOVR. Windows selects the OpenXR runtime. SteamVR remains supported as an
OpenXR runtime; forwarding to native OpenVR, OpenComposite and Elite's direct
LibOVR backend are not target transports.

Bypassing Elite's LibOVR preference is required migration work and must be
automatic in the native release. It is not a headset-specific opt-in or a
permanent choice between EDVR backends. Separate qualification builds and an
explicit rollback artifact are temporary migration tools. An upgrade must
install the complete native pair and preserve user settings; rollback restores
the previous installation deliberately rather than silently selecting a legacy
backend after OpenXR failure.

System OpenXR runtime selection and Elite's choice between LibOVR and OpenVR
are separate decisions. Changing the former does not force the latter. The
game-facing OpenVR API remains the compatibility entry into EDVR; the runtime
transport behind it is OpenXR.

## Static investigation result

The installed Frontier executable has a hard-coded backend selection loop:
LibOVR first, OpenVR second, then a third unsupported backend kind. The loop
stops on successful initialization and advances on failure. Static disassembly
now establishes the LibOVR load-failure path into that OpenVR fallback. This is
executable evidence, not a controlled launch with suppression enabled.

The inspection used the installed Odyssey executable, SHA-256
`e6be8bbe04e6a7ae226d4318945af7f367de13dc5a007a261964d9ba8144e988`. Imports,
call sites, virtual method mappings, cleanup and branch continuations were
checked with the PE reader and MSVC disassembler. Detailed offsets and
disassembly remain in the ignored `build/libovr-selection-static` archive.
These findings apply to this executable revision; future revisions need
validation. Twenty-four static import, instruction and virtual-table checks
passed against the unchanged executable, and an independent Luna review
confirmed the selection loop and load-failure cleanup. These checks do not
execute the game or qualify a loader hook.

### Entry and fallback

The installed Frontier executable's five OpenVR delay imports are
`VR_ShutdownInternal`, `VR_GetGenericInterface`, `VR_IsInterfaceVersionValid`,
`VR_GetInitToken` and `VR_InitInternal`. EDVR's facade implements this entry
path. Replacing `Openvr/win64/openvr_api.dll` only controls calls that reach
OpenVR; it cannot itself stop an earlier LibOVR choice.

The executable has no normal or delay LibOVR DLL import. Instead, its
compiled-in SDK shim constructs the LibOVR runtime filename, searches for it,
and calls the executable's regular `KERNEL32.dll!LoadLibraryW` import. On
success it resolves `ovr_Initialize` and the other SDK functions with
`GetProcAddress`.

The inspected failure chain is:

1. Both runtime search attempts fail to load a module.
2. The SDK loader returns `-3001`; the initialization wrapper propagates it.
3. Elite's Oculus backend returns false before creating an Oculus session. Its
   error-query wrapper handles an unloaded SDK, and shutdown checks the
   unresolved SDK function pointer before calling it.
4. The selection loop advances. The factory cleans up the failed backend,
   constructs OpenVR and invokes its initializer.
5. The OpenVR initializer loads `openvr/win64/openvr_api.dll` and calls the
   OpenVR entry points implemented by EDVR's native facade.

This establishes a plausible refusal point with an existing fallback. It does
not establish that a particular headset will initialize successfully through
the Windows-selected OpenXR runtime, or that a future hook executes early
enough.

Ruled out: pointing `LIBOVR_DLL_DIR` at a nonexistent directory is not a
reliable disable mechanism, because the shim subsequently tries its normal
search path. The inspected selector constructs its priority list directly; no
runtime-selection option is consulted in that function. The executable-string
and local configuration search did not establish a supported force-OpenVR
setting. This is not proof that no other setting exists elsewhere.

### Earlier EDVR interception

Elite imports `d3d11.dll!D3D11CreateDevice` normally, so the graphics proxy has
a process-attach entry before the game's executable entry point. The native
OpenVR facade arrives only if Elite reaches OpenVR, making it too late to
prevent the preceding Oculus choice.

The existing [early input gate](../src/d3d11/input_gate.cpp) demonstrates a
small executable-only IAT patch at graphics process attach. The [IAT
helper](../src/common/iat_hook.cpp) can target the confirmed regular loader
import and preserve its previous target. Device attachment is not a guaranteed
early interception point. Normal graphics initialization runs at the first D3D
export call; its ordering relative to the Oculus probe still requires a launch
trace.

Early activation is the remaining design issue. Today's development installer
uses the same graphics binary for legacy and native installs; the native
package is identified by `Openvr/win64/edvr_openxr.ini`, read by the native
module after reaching OpenVR. That arrangement must not become a permanent
per-user opt-in gate. The native release must make the graphics component's
startup-routing responsibility explicit and ensure the matching native facade
is installed.

Do not move configuration parsing, proxy-chain loading or normal logging into
`DllMain` to obtain that identity. The existing early configuration reader also
allocates and performs file I/O, so it is not a suitable primitive for a strict
loader-lock-safe hook.

The existing [runtime detector](../src/d3d11/vr_runtime.cpp) recognizes loaded
`LibOVRRT` modules; it does not suppress them. The current executable contract
check in [openxr_pe.py](../tools/openxr_pe.py) validates OpenVR imports and
cannot establish the dynamic LibOVR decision. No LibOVR suppression, global
runtime modification or Oculus library replacement has been implemented in this
checkpoint.

## Proposed implementation and next evidence gate

Prefer an executable-only `LoadLibraryW` IAT wrapper over a process-wide loader
detour or edits to Elite's code. Suppression must require a validated Elite SDK
call site and an exact LibOVR runtime filename. Every qualified native release
must enable this routing automatically, with the installer guaranteeing the
paired native facade. Unrecognized callers must forward unchanged. Unknown
executable revisions must not receive an unvalidated patch; report the
compatibility gap and do not qualify a silent return to LibOVR as successful
migration. Do not ship the local offsets as an unconditional patch.

The next implementation should first record the relevant loader calls without
changing their result. Install only the minimal IAT exchange during process
attach. Record bounded caller/path/result data in fixed storage, preserve
last-error and original-target semantics, and flush through normal logging
outside loader lock. The drain must also collect probes that occur after the
first graphics initialization. Report hook installation, missing calls and
buffer overflow explicitly so silence cannot be mistaken for success. The
wrapper must tolerate concurrent calls and must not recursively initialize
EDVR.

Use that trace to establish whether normal graphics initialization precedes the
LibOVR probe. The implementation must activate early enough for the observed
order, without depending on a user-selected backend setting. An immutable
native graphics build capability available in memory at process attach can
identify its responsibility without file I/O; the development installer and
rollback tests must distinguish that paired build from the prior proxy. This is
a rollout distinction, not a plan to maintain two shipping backends. Reading an
adjacent marker in `DllMain` or assuming the first loader call is outside
loader lock is not an acceptable shortcut.

Once early activation is established, test a refusal of only Elite's own LibOVR
probe. All other calls must forward unchanged, including matching filenames
requested by runtime modules. Exercise both shim search attempts; refusing only
the first still permits discovery through the fallback path.

If interception is necessary, scope it to Elite's own legacy LibOVR probe and
verify fallback into EDVR's OpenVR facade. A blanket process-wide ban on
`LibOVRRT` is unsafe as a design assumption: the selected Meta OpenXR runtime
could itself depend on legacy runtime components. A loader hook would need
caller filtering, reentrancy and loader-lock analysis, and launch evidence that
OpenVR initialization follows refusal. Do not rename or remove system Oculus
libraries.

Qualification must cover:

- Offline wrapper tests for caller/path/package filtering, forwarding,
  last-error preservation, concurrent publication, unknown executable behavior,
  and uninstall/rollback.
- Upgrade from each existing route (SteamVR/OpenVR, OpenComposite and direct
  LibOVR), with the SDK absent and present. Every upgraded installation must
  reach native OpenXR automatically and preserve settings.
- Explicit rollback restores the previous DLL/configuration arrangement. OpenXR
  initialization failure must not silently select a legacy runtime path.
- A connected Meta/Oculus headset with its compatible Windows-selected OpenXR
  runtime: record Elite's refused probe, subsequent native `VR_InitInternal`,
  selected runtime, rendering, recenter and clean shutdown.
- Runtime-originated Oculus component loads continuing normally; absence of
  every `LibOVRRT` module is not the success criterion.
- Quest/VDXR, Pimax/PiOpenXR and SteamVR OpenXR regression coverage, plus
  missing-runtime/startup-failure behavior.

Windows' selected runtime must actually support the connected headset; forcing
the entry path alone cannot establish compatibility for every older Oculus
device. No game launch, suppression implementation, installed DLL change or
runtime-setting change was performed for this static investigation.

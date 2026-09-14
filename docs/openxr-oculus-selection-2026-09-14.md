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

The native graphics build now carries an immutable startup capability. The
development installer selects `build/edvr_openxr_graphics.dll` and installs it
as `d3d11.dll`, paired with `edvr_openxr_runtime.dll` as the game-facing
`Openvr/win64/openvr_api.dll`. The local OpenXR configuration is still read
later by the native module; early routing does not depend on reading that file.
`build/d3d11.dll` remains a separate qualification/rollback artifact without
the routing capability. This does not introduce a permanent user choice between
shipping backends.

Do not move configuration parsing, proxy-chain loading or normal logging into
`DllMain` to obtain that identity. The existing early configuration reader also
allocates and performs file I/O, so it is not a suitable primitive for a strict
loader-lock-safe hook.

The existing [runtime detector](../src/d3d11/vr_runtime.cpp) recognizes loaded
`LibOVRRT` modules; suppression is separately owned by the [startup
route](../src/d3d11/oculus_route.cpp). Runtime module presence alone cannot
establish which entry path Elite used.

## Implemented routing and qualification

The native graphics DLL installs an executable-only `LoadLibraryW` IAT wrapper
at process attach, before ordinary graphics initialization. It refuses only
calls from the audited SDK return RVA `0x4e70bc` with the exact
case-insensitive basename `LibOVRRT64_1.dll`. This covers both search attempts
through the same SDK call site. Other names and callers continue through the
previous IAT target with its result and last-error semantics preserved.

The [mapped executable profile](../src/common/elite_oculus_profile.h) validates
PE identity, imports, the IAT slot, relative calls, relocated virtual-table
entries and eleven complete code-block fingerprints before exchanging the slot.
The [installer profile](../tools/elite_oculus.py) additionally requires the
exact executable SHA-256. Unknown revisions are rejected before install; if the
game changes afterward, startup leaves its IAT alone and reports an unsupported
profile. That condition is a compatibility gap, not successful migration. New
game revisions require a newly audited profile.

The immutable `edvrNativeStartupRouting` data export distinguishes the native
graphics artifact without loading it. [Static pair
validation](../tools/openxr_pe.py) requires the marker's exact ABI and
read-only, non-executable placement, plus the native graphics provider exports.
Both direct and receipt-based installs check the pair and executable before
writing. Older receipts remain usable for explicit rollback.

Startup and the wrapper use fixed atomic storage with no configuration reads,
logging, allocation, waits or runtime loading. Original-target and routing
identity publication precede the IAT exchange, so a concurrent first probe is
filtered immediately. Installation and removal preserve another hook owner's
slot. Normal log initialization, owned Present calls and shutdown drain bounded
changed observations into the existing graphics log under `edvr_logs`.

The `oculus route:` line reports capability, profile recognition, installation,
calls before/after log readiness, the qualified caller RVA, rejected/forwarded
counts and failures. An initial line appears even when no probe was seen. The
first actual rejection remains reportable after ordinary snapshot exhaustion.
The CPU-only `edvrQueryOculusRouting` export permits startup inspection without
initializing graphics. A refused probe followed by native `VR_InitInternal` and
the selected runtime is the required live evidence; desktop tests alone cannot
establish successful migration.

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
device. The static investigation did not launch the game. Implementation
qualification is recorded below; runtime settings and system Oculus libraries
are not modified by this route.

## Desktop and Frontier checkpoint

The full absolute-path `build.bat` run passed with all 534 source hashes
unchanged. It includes the existing ABI, lifecycle, transport, feature and
configuration gates plus 20 loader-route assertions, 17 standalone mapped
profile checks, 15 Python profile checks and 30 PE checks. Actual graphics DLL
startup inspection passed 17 checks for each of the native and baseline
artifacts. Installer self-tests cover mismatched pairs and unknown profiles
before writes, zero-write dry runs and compatibility with older receipts.

The actual Frontier executable passed the exact file validator and 43 offline
mapped-image checks. Those checks accept two distinct relocated allocations,
reject changes to every audited code block and all three virtual-table
pointers, reject changed PE identity and a partial import terminator, and
confirm the restored image passes again. No game code or imports are executed
by this fixture. Its optional command is `build/elite_oculus_test.exe --game
<absolute EliteDangerous64.exe path>`.

The new native graphics artifact also passed actual output checks for TAA and
DLSS (169 each, 18 treated eyes per mode), plus sharpening on/off (58/22).
These verify provider engagement and output, not headset image quality or
comparative performance.

Frontier is installed and hash-verified with `v0.16.2-93-g6f99b03-dirty`:

- Native runtime SHA-256:
  `98111f0c45ba256dab7175c6ed6edc3429e9c2325b4445fdcc7b85e086a6dd64`.
- Native graphics SHA-256:
  `02c7abb28f6f38f20245f1686f18d4f55fbb2756eb927e60134a37cd9facf563`.
- Local startup configuration SHA-256:
  `3da530c6e9c7e82b99a9f10caf0f6ea8b467719b36837aa4e2cbb37bd924f7b2`.
- Windows runtime discovery remains `runtime=system`; `edvr.ini` and
  `openvr_api_orig.dll` hashes are unchanged.

The ignored `build/openxr-libovr-routing-20260914/qualification.json` records
the source and artifact hashes, build log, install preview and verification.
The earlier [supported feature retest](openxr-feature-parity-2026-09-14.md)
remains pending and can be combined with this flight. The user launches
Frontier manually; no launcher automation, game launch or runtime-setting
change was performed for this checkpoint.

For the next flight, use a Meta/Oculus headset with its compatible
Windows-selected OpenXR runtime while the legacy SDK is available. Confirm that
VR and F8 appear, startup faces the splash screen, head tracking and recenter
work, and exit is normal. The graphics log must show `native=1 profile=1
installed=1` and a refused Elite probe; the native log must then identify
successful initialization and the selected OpenXR runtime. `rejected=0` does
not prove the bypass worked. Check these through `tools/edvr_log.py --target
frontier --expect-build 6f99b03`; the archived DLL hashes distinguish this
build from another dirty build at the same commit.

Then cover Quest/VDXR, Pimax/PiOpenXR and SteamVR OpenXR as available,
combining normal gameplay and the feature checklist. Live fallback and headset
behavior are still unqualified. Promoting the native pair into the release
installer, upgrading all prior backend arrangements and release-wide rollback
remain subsequent migration work; the current release installer still carries
the previous transport.

# Intel B390 planetary generation startup crash

## Status

- State: desktop startup and Game Mode workarounds verified; remediation
  closed. Compiler and earlier exit causes remain unresolved.
- Device: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, xe driver, CachyOS,
  kernel 7.2.9-1-cachyos-deckify, Mesa 26.2.4-arch3.1.
- Game: Steam 359320, installed build 25433925, Proton Experimental
  experimental-11.0-20261001-x86_64, DXVK v3.1.1-47-g685301564ea3486, flat
  1280x720. No headset or VR runtime involved; EDVR and DLSS are not loaded. No
  internal table limit identified.
- Ruled out: an older-than-25.2.4 Mesa installation as the sole explanation,
  because the fresh October 11 game log reports Intel Mesa 26.2.4 and crashes.
- Ruled out: EDVR is required to reproduce, because the game has no local
  d3d11.dll or EDVR logs and both startup dumps contain Wine's graphics DLLs.
- Ruled out: OpenGL as a startup workaround, because WineD3D rejects a required
  CreateTexture3D with E_INVALIDARG and Elite deliberately crashes before the
  generation stall. This does not test the Vulkan hypothesis.
- Ruled out: Proton Experimental alone fixes startup, because
  experimental-11.0-20261001-x86_64 with DXVK v3.1.1-47-g685301564ea3486 still
  crashes at zero percent after discarding the old DXVK cache.
- Ruled out: user_settings-only private ICD selection, because Steam already
  sets the driver variables and Proton keeps existing environment values.
- Ruled out: WineDbg is required for a usable menu, because a second fresh
  launch reached it with the persistent wrapper and no debugger.
- Ruled out: HTTP 419 initiated the pre-menu exit, because it follows
  Shutdown_Deactivate by 0.312 seconds and no earlier failure is identified.
- Evidence: one shader worker uses a full CPU core inside libvulkan_intel.so
  for over 73 seconds, while the other workers wait. The watchdog captures
  vkCreateComputePipelines. This supports prolonged CPU-side Intel pipeline
  compilation or processing, rather than a GPU or mutex wait at that moment.
- Latest run: game PID 61720 maps the installed private ICD, no system ICD;
  Proton Experimental and DXVK 3.1.1 reach the menu. DXVK cache: 544 shaders /
  11.1 MB. Private driver, runtime and cache effects are not isolated.
- Open hypotheses: expensive Intel shader register allocation exceeding the
  game's watchdog; a compiler defect producing excessive allocation work.
  Binary/source matching identifies setup_live_interference's overlap scan.
- Open question: which compiler behavior caused the earlier stall and what
  initiated the pre-menu exit. No causal fix is identified; EDVR is uninvolved.
- Temporary Proton settings were removed after exact-SHA checks. No EDVR INI
  changes. Steam Launch Options uses the verified persistent wrapper below;
  clear it to revert.
- Evidence directory: /home/deck/.cache/elite-startup-diagnostics/2026-10-11.
- Launcher follow-up: user reports the EDLaunch window stays open after
  closing. The Mesa wrapper uses execvpe and retains no Python parent. No live
  hung launcher was captured; driver involvement in its shutdown is not ruled
  out.
- Alternative: verified native MinEdLauncher v0.13.0 installed outside the
  game. Steam launch options verified; user confirms the game launches and its
  terminal closes on game exit. Command and rollback are recorded below.
- Game Mode: Elite build 91576e7e fails every Present with 0x80004005 before
  XeSS processes a scene; BG3 rejects B390 because no present queue is found.
- Confirmed omission: private Mesa 37bf953a58 was built with platforms=x11; its
  extension list lacks VK_KHR_wayland_surface. Gamescope uses Wayland.
- Matched headless XCB tests: stock Mesa + Gamescope WSI passes 60 frames;
  private Mesa + WSI crashes; private Mesa + WSI disabled passes 60 frames.
- Corrected build: same commit with X11 and Wayland installed; host/runtime
  loading and 60-frame WSI presentation pass. User confirms both Elite and BG3
  work in Game Mode with existing launch options. No global driver changes.

## October 11 evidence

The user reproduced the startup crash at approximately 06:17 MDT. The game
starts at 06:15:38; Frontier writes its dump about 84 seconds later. DXVK
initializes successfully on B390, reports shaderFloat64 support and creates the
swapchain. Its log contains no device loss or fatal shader error. The kernel
has display pipe DSB errors, but no GPU reset or out-of-memory event identified
at the crash. These absences do not prove a correct GPU path.

Frontier.CrashReport.11.10.2026-06.17.01.dmp has no standard exception stream.
The watchdog names ThreadID 896 (0x380), but its saved context is in
win32u.dll+0x150b4. The return into Elite+0x553d16 follows a normal
MsgWaitForMultipleObjects call; this is not a verified faulting instruction.
Custom stream 0xfff0 is a native module list, including libvulkan_intel.so,
rather than fault details.

One dxvk-shader-h worker is inside winevulkan.dll+0x100ef at
vkCreateComputePipelines, with DXVK return offsets +0x17cecc, +0x144fa2 and
+0x178fa7. The October 5 dump repeats those offsets and the main-thread wait.
This supports investigating a compilation stall, but does not identify an
offending shader or a Mesa fault. A native worker stack during the stall would
distinguish compiler work from a Vulkan or synchronization wait.

## OpenGL comparison

The OpenGL run fails about six seconds after launch, before the original stall.
Its game-authored dump contains an exception stream: thread 0x328, c0000005,
null write, game+0x34c9322. Disassembly shows an intentional null write trap.
The F3D crash log records CreateTexture3D returning 0x80070057 (E_INVALIDARG),
from f3dResource_DX11.cpp(456), with device remove reason S_OK. WineD3D's
depth-slice warning precedes the failure, but its causal role is unproven. The
ID3D12 warning prefix is generic; the call is DX11.

This comparison does not distinguish the original causes. Its logs and
restoration copy are preserved in opengl-comparison. Vulkan was restored before
another test was requested.

## Proton Experimental comparison

The user again reports zero percent. Logs verify
experimental-11.0-20261001-x86_64 and DXVK v3.1.1-47-g685301564ea3486 on the
same Mesa 26.2.4 driver. DXVK discarded the older v2.7.1 cache automatically;
the stall persists with the new version and cache. The latest watchdog dump is
Frontier.CrashReport.11.10.2026-06.41.25.dmp.

Native observation of game PID 25239 shows thread 25301, dxvk-shader-h,
continuously runnable from 35 to 108 seconds. With 100 clock ticks per second,
its CPU ticks increase from 709 to 8014 over that interval, approximately one
full core. The 45-second stack places its PC at 0x7f0acaf7b50f, Intel ICD ELF
offset 0x97b50f. Earlier compiler workers sampled within 0x1f of that location.
The other shader workers are waiting by 45 seconds. The watchdog's saved
context places one worker in winevulkan.dll+0x1102f, vkCreateComputePipelines
after the Wine Unix dispatch call, repeating the stable Proton API location.

The loaded /usr/lib/libvulkan_intel.so has build ID
63f230b320e0d6489fc30f11cc74223efc3534c8. It is stripped, so nearest-exported
symbol names do not identify the internal function. The captures support active
CPU-side Intel pipeline work; they do not establish an infinite loop, the
offending shader, or a specific compiler pass. Native stacks, maps and timing
records are preserved under proton-experimental in the evidence directory.

## Intel compiler identification and proposed comparison

Matching debug symbols are unavailable for this CachyOS build ID from Arch,
CachyOS and elfutils debug services. The driver's disassembly at
0x97b490-0x97b53d structurally matches Mesa 26.2.4's
brw_reg_alloc::setup_live_interference(unsigned, brw_range). Both hot PCs
0x97b50f and 0x97b52e are within its virtual-register overlap scan. Loop
bounds, live-range tests and the interference-bit helper match the source. This
is binary/source identification, not a debug-symbol-verified stack trace.

build_interference_graph calls the scan for each virtual register; the scan
checks prior registers. The phase therefore performs quadratic candidate
overlap checks. This explains a plausible source of expensive compilation, but
does not establish a malformed shader, an infinite loop or the exact cause of
excessive work.

The user suggested Mesa 26.3. On October 11, the official Git remote reports
main at 37bf953a58d8baa6c29142c714f46418d4d596a4 and no 26.3 branch; the first
26.3 release candidate is scheduled for October 14. Prepare an isolated Intel
Vulkan build from that main commit and verify its version and driver path
before another startup. Keep the system graphics libraries intact. SPIR-V
capture is available if needed later, but disabling Mesa's cache for capture
would perturb timing during the driver comparison.

The private build and prefix install passed. Native vulkaninfo and a second
check inside SteamLinuxRuntime_4 both enumerate only B390 and report Mesa
26.3.0-devel (git-37bf953a58). The container check set VK_DRIVER_FILES after
container startup, matching Proton user_settings.py's timing. The build has X11
WSI; the baseline game loads winex11.drv. No system libraries or packages were
replaced. Source, private build tools and prefix are under
/tmp/edvr-mesa-main-20261011. The ICD is
install/share/vulkan/icd.d/intel_icd.x86_64.json, and its library is
install/lib/libvulkan_intel.so. Build logs are in that root directory.

The private driver is selected only for SteamAppId 359320 in Experimental's
owned temporary settings. Logs and the logging-only restoration copy go to
mesa-main-37bf953a in the evidence directory. The launcher was reopened and the
native observer armed; the user was asked to press Play. Confirm the game log's
Mesa version and native mapped library before interpreting this startup.
Upstream main changes register-selection and spill logic, but the hot overlap
scan's body remains unchanged; a fix is not established by building
successfully.

## Invalid driver comparison and eventual generation completion

The user first reported another zero-percent crash, then reported generation
eventually finished after leaving the game running and an exit before the main
menu. This attempt used system Mesa 26.2.4: the game DXVK log identifies that
version, and both native map snapshots identify
/run/host/usr/lib/libvulkan_intel.so inode 109170. No private Mesa-main library
is mapped. The result does not refute or confirm the proposed Mesa-main
comparison.

SteamLinuxRuntime_4 supplies both VK_DRIVER_FILES and VK_ICD_FILENAMES before
Proton starts. Proton's init_session only takes a user_settings value if its
key is absent from the existing environment. Therefore the private ICD request
in user_settings.py was ineffective. A new container check passed the private
ICD before container startup and selected Mesa 26.3.0-devel. Validate this
route through Wine dxdiag before another Elite attempt. The ineffective setting
has been removed and the owned logging-only settings restored.

The latest watchdog dump, Frontier.CrashReport.11.10.2026-06.59.34.dmp, again
has no exception stream; its main thread is in win32u.dll+0x150b4. WatchDog
reports heartbeat reset failure around 96 seconds after game launch. Network
activity continues afterward, and the game logs Shutdown and cleanup around 263
seconds. No second dump, later fatal game SEH or device loss was identified.
The user's later observed exit remains unexplained; do not assume an additional
access violation or GPU fault. The newest Proton header reports kernel
7.2.9-2-cachyos-deckify, unlike the earlier baseline header.

## Corrected Wine preflight and offline replay

Two Wine DXDiag runs in disposable Proton prefixes under
/tmp/edvr-mesa-wine-preflight-20261011 passed with the private ICD selected
before SteamLinuxRuntime_4 started. Native Wine maps identify
/tmp/edvr-mesa-main-20261011/install/lib/libvulkan_intel.so. DXVK labels its
driver 26.2.99, the encoded development driver version; Vulkan driverInfo
reports Mesa 26.3.0-devel (git-37bf953a58). Both are expected for this build.
The real game prefix and Steam configuration were not edited by this check.

The temporary /tmp/elite_mesa_main_launch.py wrapper sets both Vulkan driver
variables before Steam's command, keeps logs in mesa-main-verified-37bf953a,
and enables passive SPIR-V capture without disabling Mesa's cache. It verifies
the private library's SHA256 before execution. Dry-run wrote nothing; an
argument-forwarding/environment check passed. The user set /usr/bin/python3
/tmp/elite_mesa_main_launch.py %command% in Elite's Steam Launch Options.
Launcher PID 44297 mapped the private Intel library; its DXVK log reports
26.2.99. The current game run and its outcome are recorded below. The observer
allows ten minutes after game detection, records selected graphics environment
values, and takes native stacks at 15, 45 and 120 seconds.

Steam's first-run Fossilize archive, modified at 06:18, contains 86 compute
pipelines. A SHA256-verified copy was replayed compute-only on system Mesa
26.2.4 with one worker, a ten-second pipeline timeout, a 120-second overall
limit and a separate Mesa cache under /tmp/edvr-elite-offline-replay-20261011.
All 86 compiled in about 27 seconds, with no timeout or crash. Steam's cache
was not changed. Fossilize normally records after successful compilation, so
the original in-flight pipeline may be absent; this is not a refutation of the
observed Intel compiler bottleneck. The larger archive's 1,954 compute
pipelines have not been replayed.

## Private Mesa game comparison

The user's private Mesa run loaded
/tmp/edvr-mesa-main-20261011/install/lib/libvulkan_intel.so in game PID 44975.
Proton Experimental used DXVK v3.1.1-47-g685301564ea3486. DXVK reports driver
26.2.99, while Vulkan driverInfo identifies Mesa 26.3.0-devel (git-37bf953a58).
Generation completed, then the game automatically exited before the menu.

One shader worker's CPU activity tapered off and was idle by about 90 seconds.
Native stacks sampled jay_schedule, jay_compile_simd and anv_shader_compile.
WatchDog reported the target exited with code 0; there was no new crash dump or
device loss. The cache contained 224 shaders (5.3 MB), compared with an earlier
cache of 128 shaders (3.0 MB). This result does not show that Mesa alone
removed the original stall; the original system-driver compiler hypothesis
remains supported for that earlier run. The cause of the latest pre-menu exit
is unresolved.

Passive SPIR-V capture recorded compute shader
`cs.40d340de45f947098a595336f8e1520a` as a candidate, but no pipeline link to
the observed work is established. No Float64 use was found. NetLog records
Shutdown_Deactivate at monotonic 115.086 seconds and HTTP 419 at 115.398
seconds, 0.312 seconds later. With no earlier failure identified, HTTP 419 is
ruled out as the initiating event. Launcher ProjectFinished occurs after
shutdown and is logged after the fact.

## Verified normal startup workaround

The first menu reach used WineDbg; its game PID 55534 mapped the private ICD,
and no exit breakpoint fired. The observer detached safely while the game
remained alive. A second fresh launch without debugger also reached the menu.
Game PID 61720 maps
/home/deck/.local/share/elite-mesa-37bf953a58/lib/libvulkan_intel.so, with no
system ICD. DXVK reports 26.2.99 and 544 cached shaders / 11.1 MB.

The installed library's SHA matches the private build, has no /tmp dependency,
and native plus Runtime 4 Vulkan checks report Mesa 26.3.0-devel
(git-37bf953a58). The verified wrapper is
/home/deck/.local/share/elite-mesa-37bf953a58/launch.py; Steam Launch Options
is `/usr/bin/python3 /home/deck/.local/share/elite-mesa-37bf953a58/launch.py
%command%`. It sets only the two Vulkan ICD variables. Temporary Proton
settings are removed. Proton Experimental, the private driver and warmed caches
are a verified working combination; their individual effects and the original
compiler/exit causes remain unresolved. No EDVR changes were needed.

## Sources

- [Mesa 25.2.4 release notes](https://docs.mesa3d.org/relnotes/25.2.4.html)
  list an Intel Elite crash at zero percent generation. The current driver is
  newer; the release note alone does not identify this crash's mechanism.
- [Proton runtime
  options](https://github.com/ValveSoftware/Proton#runtime-config-options)
  document PROTON_USE_WINED3D as a reversible OpenGL renderer comparison.
- [Mesa Intel allocator
  source](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.4/src/intel/compiler/brw/brw_reg_allocate.cpp#L389)
  contains the identified overlap scan.
- [Mesa release calendar](https://docs.mesa3d.org/release-calendar.html)
  schedules the 26.3 release candidates.
- [Mesa local-build guide](https://docs.mesa3d.org/install.html) documents a
  private prefix and Vulkan ICD selection.
- [Fossilize capture
  source](https://github.com/ValveSoftware/Fossilize/blob/master/layer/dispatch.cpp#L893)
  records ordinary compute pipelines after successful driver compilation.

### Launcher shutdown follow-up and MinEdLauncher installation

The user reports that the visible EDLaunch window stays open after closing and
has to be killed. Read-only inspection of the persistent Mesa wrapper shows it
sets VK_DRIVER_FILES/VK_ICD_FILENAMES and calls os.execvpe: it leaves no Python
waiting parent, spawns no background process and has no wait loop. This rules
out wrapper-parent retention, not a Mesa/Proton shutdown problem inside the
launcher. Inspection after the user closed the session found no EDLaunch, game,
Proton or wineserver process; Steam recorded removal of app 359320 from its
running list at 12:21:03. No live shutdown hang was captured and no causal
launcher fix is claimed.

At the user's request, installed the official [MinEdLauncher v0.13.0 Linux
release](https://github.com/rfvgyhn/min-ed-launcher/releases/tag/v0.13.0) in
`/home/deck/.local/share/min-ed-launcher-v0.13.0`, outside the game. Its
archive SHA-256
c50a2c8bd3ffad8876e30f05c9e80c1ee55c43c79455e7521ef2cdd2f88367a8 matches the
release shasum and GitHub asset digest; GitHub attestation verification exits
0. Dry-run creates nothing, installation verifies all five copied release files
and preserves executable mode. This is a self-contained Linux application; no
system .NET installation is needed. Native startup displays v0.13.0+6e50d0e0
and correctly rejects missing game-location arguments in a sandbox smoke test.
The initial smoke test needed a temporary DOTNET_BUNDLE_EXTRACT_BASE_DIR
because the sandbox cannot write the normal home cache. No such override is
installed for Steam.

The [exact-release
README](https://github.com/rfvgyhn/min-ed-launcher/tree/v0.13.0) documents the
alacritty Steam command, /edo, /novr and /autoquit waitForExit. The
exact-release Settings.fs discovers Elite from EDLaunch.exe in %command% and
preserves the preceding Steam/Proton command, allowing the standalone
installation. Activated Steam command preserves the Mesa wrapper and Proton
Experimental:

```text
/usr/bin/python3 /home/deck/.local/share/elite-mesa-37bf953a58/launch.py /usr/bin/alacritty -e /home/deck/.local/share/min-ed-launcher-v0.13.0/MinEdLauncher %command% /autorun /autoquit waitForExit /edo /novr
```

Steam's saved Elite launch options match this command. The user confirms the
game launches and the terminal closes after quitting Elite. No live Steam VDF
was edited by the agent. Previous launch option, also the rollback command:

```text
/usr/bin/python3 /home/deck/.local/share/elite-mesa-37bf953a58/launch.py %command%
```

The installed Mesa driver, wrapper and flown 4b7bfec4 EDVR files/settings
remain unchanged. Main stays 99476a02; XeSS/UI diagnostic cleanup 02f50bd7 is
pushed on codex/xess-flat with native Windows full validation pending.

## October 11 Game Mode presentation follow-up

Desktop tests succeeded, but Game Mode gives Elite a white screen with audio
and BG3 reports `graphics queue does not support presenting and present queue
not found`. Elite's 15:03:19 flight log matches the installed clean 91576e7e
build and fails its first and subsequent Present calls with 0x80004005. It
records zero treated scenes. Ruled out: XeSS scene processing causes this white
screen, because presentation fails before any scene reaches XeSS.

The private Mesa build's `intro-buildoptions.json` sets platforms to `x11`
only. Its host Vulkan extension list includes XCB/Xlib surfaces but omits
VK_KHR_wayland_surface. The working desktop path therefore did not validate the
Wayland path required by Gamescope's WSI layer. Hypothesis: the missing Wayland
platform prevents private-driver presentation in Game Mode. Confirm or refute
by building the same commit with both platforms and repeating the matched
headless Gamescope XCB test with its WSI layer enabled.

Three explicit client-return-code tests isolate a driver/layer interaction:
stock Mesa 26.2.4 with WSI enabled presents 60 frames and exits 0; private
37bf953a58 with WSI enabled exits -11 after surface creation; private Mesa with
DISABLE_GAMESCOPE_WSI=1 presents 60 frames and exits 0. Evidence is in
`/tmp/edvr_gamescope_headless_{stock_xcb,private}_status_*.log`. The headless
tests have no DRM output. Ruled out: DRM AddFB2 failures explain this client
test crash, because it reproduces without the DRM path. Those compositor errors
predate both games, so a Game Mode flight is still required even if the
corrected driver passes the headless test. No wrappers or settings have been
changed during diagnosis.

The isolated corrected build completed successfully. Its source remains clean
at 37bf953a58; Meson options differ only in platforms (`x11,wayland`), staging
prefix and the pkg-config path for the data-only Wayland protocols dependency.
Protocols 1.41 are pinned to official GitLab commit
71da8bd7f9fd3ea7c5d87ad19ca087eeca137673 and installed under `/tmp`; no system
packages changed. The new library SHA256 is
470baad14a458382b3d9ae08440be1901a1f2ae6ecc73cf67892b7b8b8838daa.

Host and Steam Runtime4 Vulkan summaries both identify B390 and the same Mesa
revision, expose VK_KHR_wayland_surface and exit 0. Matched headless XCB
clients now create a swapchain and present 60 frames with Gamescope WSI
enabled, exit 0; WSI disabled also exits 0. This confirms that adding the
missing platform corrects the reproduced presentation failure, without
disabling the layer. Logs are
`/tmp/edvr_mesa_wayland_{host,runtime4}_summary.log` and
`/tmp/edvr_gamescope_headless_wayland_xcb_status_{enabled,disabled}.log`. The
user subsequently confirmed that both Elite and BG3 work in Game Mode.

Guarded installation replaced only the private library and its version.json
library hash, retaining the old files as rollback backups. Independent SHA
checks confirm both wrappers, rear-button helpers, ICD, system Mesa and both
live/mirrored flat INIs unchanged. The existing generic wrapper then loads the
installed driver inside Steam Runtime4, reports the correct B390/Mesa revision
and Wayland extension, exit 0. Evidence:
`/tmp/edvr_mesa_wayland_installed_runtime4_summary.log`. The user confirms that
both Elite and BG3 work in Game Mode with their existing Steam launch options.
This closes the presentation follow-up. The new cockpit/scene AA disengagement
reported during that flight belongs to the XeSS/flat arc.

Retained backups beside the original files:
`lib/libvulkan_intel.so.edvr-rollback` and `version.json.edvr-rollback`. The
guarded helper `python3 -I -B /tmp/edvr_private_mesa_library.py --rollback
--new-sha256 470baad14a458382b3d9ae08440be1901a1f2ae6ecc73cf67892b7b8b8838daa`
restores only those two files after validating their exact identity and
checking that neither game is running. The helper is temporary; the backups are
in the persistent private-driver directory.

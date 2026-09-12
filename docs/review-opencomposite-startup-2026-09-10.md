# OpenComposite startup and Quest menu investigation

## Evidence

The two supplied archives are `edvr-logs-20260910-205646.zip` and
`edvr-logs-20260910-210909.zip`. Both contain v0.15.0 logs; the release
tag resolves to 188ffbd, the source reviewed here. Graphics build
6AA345C5 and OpenVR build 6AA345CD are separate link timestamps, not a
version mismatch. The game reports build 332841.

The newer run positively identifies OpenComposite by runtime exports.
It recommends 2528x2704 per eye, with parallel eyes at +/-0.0319 m.
The configuration selects DLSS preset K and HMD Quality 1.25, so the
first trained feature is DLAA at 2528x2704. Present and Submit both run
on thread 20584. There is no observed cross-thread context use here.

The older archive's two crashes occur before any recorded compositor
installation, at Elite RVAs 0x5CBFC8 and 0x5CB322. They must not be
attributed to a DLSS evaluation that had not started. On the local Steam
executable, also build 332841, these are indexed memory reads. The logs
do not contain registers, access parameters, or a minidump sufficient
to explain those original access violations.

The newer archive has four repeated crashes at Elite RVA 0x34C9322.
The local executable has `xor eax,eax; mov [rax],eax; ret` at
0x34C9320: a deliberate null write. Its position in the EXE does not
exonerate a mod or prove spontaneous memory corruption. The latest
flight supplies a much more useful sequence:

| Time | Observation |
|---|---|
| 21:08:18.983 | First graphics device, feature level 0xC000 |
| 21:08:18.987 | Recovery sentinel declines graphics hooks |
| 21:08:21.210 | Second device, feature level 0xB000 |
| 21:08:21.229 | Graphics hooks nevertheless installed on device two |
| 21:08:22–51 | Temporal shaders compiled on its context |
| 21:08:55.065 | IVRCompositor_014 hook installed |
| 21:08:55.691 | First temporal eye processing |
| 21:08:56.952 | DLAA evaluation fails, NGX 0xBAD00002 |
| 21:08:56.952 | Native fallback textures cannot be created |
| 21:08:56.952 | Resubmit texture creation returns 0x887A0005 |

0x887A0005 is DXGI_ERROR_DEVICE_REMOVED. It does not distinguish invalid
commands, a hang, or a reset by itself. Microsoft's recommended next
measurement is [GetDeviceRemovedReason](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-getdeviceremovedreason).
The failed NVIDIA evaluation now records that result and the device.
The logs do not establish overclocking as the cause.

## Recovery defect and fix

`Sentinel::clearTrip()` clears both the file and the in-memory flag.
`hookDevice()` returned after calling it, but did not latch the refusal.
The next device creation therefore installed hooks on that device.
Meanwhile the first game's swapchain drove the frame work, and shader
warming used the newly hooked device's context. Temporal submission
obtains its context from the submitted texture but formerly reused the
global warmed shaders without verifying their owning device.

This is a demonstrated recovery defect and an unsafe resource ownership
path. The supplied logs do not record the submitted texture's device
identity, so they alone do not prove which GPU command caused removal.

The fix latches recovery for the whole process, including factory and
swapchain hook attempts and the temporal export. It releases the early
keyboard interception, since there is no menu to consume input in this
session. The log explicitly explains that the menu is unavailable until
the next launch and that OpenVR has a separate recovery guard.

Temporal resources also retain their owning device. A different device
is refused before cached queries, bindings, copies or dispatches are
used. This is a conservative refusal, not support for switching graphics
devices while preserving temporal history.

The regression fixture creates two real D3D11 devices after arming the
sentinel. Against the unmodified 188ffbd DLL it reproduces the defect:
clearing grey reads back 32,32,32 on device one but 0,0,0 on device two.
The graphics log confirms a sentinel refusal followed by hook installation.
The fixed build must preserve grey on both and refuse temporal treatment
on both. A separate fixture initializes temporal rendering on one device,
refuses the second, and verifies that the original device still renders.

## Quest 3 menu projection

OpenVR's historical `pfTop`/`pfBottom` names refer to the physical -Y/+Y
edges respectively. The shared channel preserves that API order.
`intro_panel.cpp` correctly reconstructs the projection matrix from it;
the menu instead used its first magnitude as physical up in its ray shader.
The temporal pass already uses the correct convention.

For the measured Quest frustum (raw top -1.4281, raw bottom +0.9657),
the runtime matrix has m12 about -0.193. The menu's reversed ray pair
produced the opposite center shift: about 19.3% of the image height
between the intended and displayed center at a level head pose. Rotating
this incorrect ray into the fixed anchor produces apparent deformation.
Symmetric vertical frusta conceal the error.

The menu now converts the channel's API order to physical up/down at its
frustum boundary. Its anchor continues to follow the summon look direction
with roll removed; no positional compensation or new setting is added.
The test projects points with the independently constructed runtime matrix
and recovers panel coordinates through the production menu functions:
324 points across both eyes, yaw, pitch, translation, flat and curved
panels. Both the shader and its culling box use the corrected frustum.

The convention is also documented in the
[OpenVR API discussion](https://github.com/ValveSoftware/openvr/issues/110).

## F8

The reporter subsequently confirmed that another hotkey opens the menu.
That rules out a universally unavailable menu for their working session.
F8 goes through the same configurable binding and scan-code path as the
other keys. The current evidence cannot distinguish an external shortcut,
keyboard Fn mode, or a focus difference. Keep their working binding.
Menu polling now reports a received summon press rejected for lack of
desktop focus, using the existing hotkey diagnostic flag. The default is
unchanged. This does not claim to identify or fix an external F8 conflict.

Sean also tested the existing build with OpenComposite, Virtual Desktop
and Quest 3: no crash, and F8 worked. Neither issue is universal to that
runtime/headset combination. His performance monitor appeared skewed;
it uses the same corrected panel projection as the menu.

## Compositor statistics

The upstream OpenComposite OpenXR backend's
[GetFrameTiming implementation](https://gitlab.com/znixian/OpenOVR/-/raw/openxr/DrvOpenXR/XrBackend.cpp)
fills GPU times and drop/reprojection counts with constants, not measured
compositor statistics. Its clock is epoch-based, outside EDVR's validated
uptime range. Accepting the layout by relaxing that range would expose
fabricated timings. Older builds can instead refuse the query entirely.
The Monitor tab now explains absent runtime values as
`OpenComposite: unavailable`. FPS, measured thread time and EDVR's own
GPU queries remain independent; these do not substitute for full scene
GPU timing or the compositor's drop count. No source of real OpenXR
compositor measurements has been added in this change.

## Removed drive suppression settings

At Sean's request, `fix.heat_haze` and `fix.drives_smoke` are removed from
the shipped INI, generated menu, and active code. This removes their
shader selectors, counters, direct/indirect draw suppression and obsolete
suppression test tool. The engine now receives those draws in TAA/DLSS
as well as with AA off. Old INI entries have no effect and the existing
config audit reports them as retired/unread; a user's other settings
are preserved on installation.

The private smoke-depth correction and the particle billboard treatment
remain. Removing a visibility switch must not undo the separate fix for
rectangular holes under temporal AA. Earlier smoke investigation documents
describe historical builds; their recommendations to use these two switches
are superseded by this removal.

## Validation

The initial full build and its gates passed, including the 324-point
Quest projection check. Reinstating the old vertical ordering in a copied
source file makes precisely that check fail. The two-device recovery
regression fails on 188ffbd and passes with the fix; the foreign-device
test refuses the second device and confirms the original remains usable.
Both devices report S_OK from GetDeviceRemovedReason. The complete GPU
smoke test passes, including native TAA, DLAA, DLSS and crop motion checks.
After removing the two suppression settings, the final full build,
recovery test, foreign-device test and GPU smoke test also passed.
Artifacts are under build/review_crash (`build-final.log`,
`recovery-final.log`, `foreign-final.log`, `smoke-final.log`).

The tested DLL pair was installed to Sean's Frontier installation with
tools/install_edvr.py at 19:48 local time on September 10. Both file
hashes were verified against the build; the INI and original runtime
hashes were unchanged. Backups carry the `oc-review-20260910` tag.
The installed SHA256 values are:

* d3d11.dll: `6e2616b878899df85f837de2361bec84ef7429ac5d06f4143a893e84935a5b2c`
* openvr_api.dll: `d4d66d0fc2ad8184b17c5da021d1d03161f920143959e3782a59b948d99f82c5`

## Limits and next flight

Ruled out: mismatched EDVR release versions; both halves are v0.15.0.
Ruled out: the older pre-compositor crashes being caused by an observed
DLSS evaluation; no evaluation had been logged in those runs.
Ruled out: ignoring menu pitch as the Quest placement cause; the anchor
already preserves look pitch, and the projection test isolates the error.

Neither archive contains OpenComposite/OpenXR runtime diagnostics or a
minidump. If a clean launch still fails, collect the new EDVR bundle and
the runtime's own logs; the new device-removal result will distinguish
the graphics failure more precisely. The older CPU exceptions remain
unexplained by the available capture. Verify the menu in a Quest session
by summoning while looking up and sideways, then moving the head while
keeping the panel in view.

The reporter uses Steam Link, and subsequently reported a successful
retry with OpenComposite. Ruled out: an inevitable crash whenever
OpenComposite is used, because both Sean's separate test and the reporter's
retry succeeded. If SteamVR is selected as his OpenXR runtime, the chain
is Elite -> EDVR -> OpenComposite -> SteamVR -> Steam Link; that differs
from a Virtual Desktop session selecting VDXR. The selected OpenXR runtime
has not been captured, so this chain is conditional. A controlled comparison
can keep Steam Link and EDVR while bypassing OpenComposite. There is no
evidence here that his runtime combination is inherently invalid.

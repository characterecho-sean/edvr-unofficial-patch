# DXVK on Windows: what integrating it would take

*A design exploration, written from reading the code and the external record.
Nothing in it is implemented, and nothing in it has been run against the
game. Claims about EDVR cite the source; claims about DXVK and SteamVR cite
where they came from; the things that can only be settled by running the
combination are collected in one list near the end, labelled as such.*

[DXVK](https://github.com/doitsujin/dxvk) is a Vulkan-based implementation
of D3D11 (and 8/9/10). It exists for Wine, but its Windows builds work as
drop-in replacements: put its `d3d11.dll` and `dxgi.dll` next to a game's
executable and the game renders through Vulkan. People do this to Elite
Dangerous on Windows for two reasons that both matter more in a headset than
on a monitor: smoothing the frame pacing of a CPU-bound renderer (Odyssey
settlements are the standing example), and escaping weak vendor D3D11
drivers (Intel Arc being the current one — an
[ED: Odyssey on Arc B580 report](https://github.com/doitsujin/dxvk/issues/5492)
runs, on Windows, with one config workaround).

The short version of the whole document:

- **Flat screen: EDVR already has the mechanism.** DXVK is "another mod
  that wants to be `d3d11.dll`", and `advanced.real_dll` exists for exactly
  that shape. Chaining it should need no code — an install recipe, a test
  pass, and one log nicety.
- **VR: chaining is not enough, and the reason is structural.** The game
  hands SteamVR its eye texture as a D3D11 texture pointer. Under DXVK that
  pointer is a DXVK object, and the sharing machinery SteamVR's D3D11 path
  needs does not exist for DXVK on native Windows. The fix with precedent
  is to translate the submission to Vulkan — which is what Valve's own
  Proton does for this exact game on Linux — and EDVR's compositor hook is
  already standing on the one line of code where that translation goes.
- **The honest cost is verification and support, not code.** The bridge is
  a few hundred lines against a stable interop ABI. What it buys is a new
  axis of environmental variance (DXVK version × GPU driver × runtime) in a
  project whose support model is "the log is usually the whole answer".

---

## Flat screen: chaining, which mostly exists

### The name collision and the existing answer

DXVK's Windows install is two files next to `EliteDangerous64.exe`:
`d3d11.dll` and `dxgi.dll`. EDVR is already the `d3d11.dll` there. This is
the EDHM collision again, and the EDHM answer holds:

1. DXVK's `d3d11.dll` goes in **renamed** — say `d3d11_dxvk.dll`.
2. DXVK's `dxgi.dll` keeps its own name. EDVR does not proxy dxgi, so
   there is nothing to collide with.
3. `edvr.ini`: `[advanced] real_dll = d3d11_dxvk.dll`.

The chain loader (`chainThroughOtherProxy`, `src/d3d11/d3d11_proxy.cpp`)
was built for this: it loads the configured DLL after the loader lock is
released, refuses it unless it exports `D3D11CreateDevice` (DXVK does, plus
`D3D11CreateDeviceAndSwapChain`), resolves every export from it with
per-export fallback to the system `d3d11.dll` for anything it lacks, and
logs `forwarding to <path>` from the module itself. The re-entrancy depth
guard that keeps 3Dmigoto-style proxies from looping applies unchanged.

The DLL-name resolution works out consistently, which is the part worth
stating rather than assuming: the game's own `dxgi.dll` reference resolves
to the app-directory DXVK copy (dxgi is not a KnownDLL — this is why DXVK
works as a drop-in at all), and when EDVR later chain-loads
`d3d11_dxvk.dll`, that module's import of `dxgi.dll` binds to the *already
loaded* module of that name — the same DXVK dxgi. One factory, one adapter
family, one device implementation. The reverse mistake — DXVK's d3d11
without DXVK's dxgi — fails at device creation because DXVK's d3d11 needs a
DXVK adapter; the install recipe has to say both files or neither.

### What the d3d11 half assumes, checked piece by piece

Everything EDVR does on the graphics side is COM-ABI-level D3D11, which
DXVK implements:

- **Hooked slots are frozen COM ABI.** Device `CreateComputeShader` (18),
  swapchain `Present` (8), factory `CreateSwapChain`/`ForHwnd` (10/15),
  and the context slots (`Map`/`Unmap`, draws, `OMSetRenderTargets`,
  `ClearRenderTargetView`, `Dispatch`, `CSSetShader`/`UAVs`,
  `ExecuteCommandList`, `ClearState`). Interface layout is the contract,
  not the implementation behind it.
- **The hook-mode probe lands on the safe answer by construction.**
  `contextHookModeFor` counts vtable entries inside *Windows'* d3d11.dll;
  a DXVK context samples ~0 of 96 and takes `InPlace` — the mode that
  composes with foreign implementations, the EDHM/ReShade-proven path.
  (The 2026-08-18 lesson that forced `CopyVptr` for Microsoft's runtime —
  the runtime re-pointing its own shared table — is a Microsoft-runtime
  behaviour; DXVK's vtables are ordinary static C++ tables.) The log will
  say `0 of 96 ... InPlace`, which is correct but reads like a wrapper
  detection; worth one added line naming DXVK when DXVK is what it found.
- **EDVR's own GPU work is plain API.** One small constant buffer plus
  `Map(WRITE_DISCARD)` (panel distance), `CopyResource` between matching
  resources (exposure share), `GetDesc`/`CreateTexture2D`/
  `CopySubresourceRegion` (resubmit shadow, cull-guard crop). No shared
  handles, no queries, no deferred contexts, nothing device-removed-exotic.
- **The fixes that never touch D3D are untouched by DXVK**: the resolution
  patch and camera marker scan work on the game's code and memory; journal
  and bindings watching work on files.

Two Windows-specific DXVK caveats belong in any user-facing text, both from
[DXVK's own Windows page](https://github.com/doitsujin/dxvk/wiki/Windows):
games that tightly integrate NVAPI/AGS can crash under it (Elite is not
known to, but "not known to" is the honest phrasing), and upstream does not
test AMD or Intel *Windows* drivers — DXVK-on-Windows is a tolerated use,
not a target. DXVK 3.x requires Vulkan 1.4-level drivers
([3.0 notes](https://github.com/doitsujin/dxvk/releases); NVIDIA 595.84+,
with AMD RDNA1/2 on Windows explicitly called out as problematic at
release); 2.7.1 remains the fallback for older stacks. And the Frontier
launcher's verify step will delete foreign DLLs exactly as it already
deletes EDVR's — same fact, one more file.

So: flat screen is an afternoon of testing and a README section. It is
also, for this mod, nearly beside the point — every fix EDVR ships is a VR
fix. The integration question is the next section.

## VR: where chaining stops working

### The problem, stated precisely

In VR the game calls `IVRCompositor::Submit` with
`TextureType_DirectX` and its eye texture's `ID3D11Texture2D*`. EDVR's
compositor hook already stands in that call (`hookedSubmit`,
`src/openvr/compositor_hook.cpp`): it reads the texture's size, keeps a
shadow copy for the flash fix, crops for the cull guard — all through the
game's own device, all of which keeps working under DXVK, because in-process
the handle is a real COM object and DXVK implements what EDVR calls on it.

Then the hook forwards to the real `openvr_api.dll`, and the pointer leaves
the world where "implements the interface" is enough. SteamVR's client
library has to get those pixels to the compositor *process*, and its D3D11
path does that with Windows graphics-kernel sharing — which is the one
thing a DXVK texture does not have on native Windows. DXVK's shared-resource
support is real but asymmetric: the
[D3DKMT-based implementation](https://github.com/doitsujin/dxvk/pull/5257)
(merged November 2025) states it plainly — *importing* shared resources
works on both platforms; *creating and exporting* them needs Wine-side
mechanisms that native Windows lacks. A DXVK-rendered frame can consume the
world's shared textures; it cannot hand its own across a process boundary
the way SteamVR's D3D11 path expects.

The field record matches the theory: Windows players report DXVK working
for Elite on the flat screen and
[not working in VR](https://steamcommunity.com/app/359320/discussions/0/595152438140395018/).

### The fix with precedent: submit Vulkan instead

SteamVR speaks Vulkan natively, on Windows too — `TextureType_Vulkan`
submission with a `VRVulkanTextureData_t` (VkImage plus the instance,
physical device, device, queue and queue family it lives on, width, height,
VkFormat, sample count). And a DXVK texture *is* a VkImage; DXVK exposes it
through stable interop interfaces on its DXGI objects
([`IDXGIVkInteropSurface`, `IDXGIVkInteropDevice`](https://deepwiki.com/doitsujin/dxvk/7-dxgi-implementation)):
query the submitted texture for its interop surface, get the interop
device, `GetVulkanHandles` / `GetSubmissionQueue` / `GetVulkanImageInfo`,
flush and transition the image, lock the submission queue around the
compositor call, release, transition back.

This is not a design that needs inventing. It is, call for call, what
Valve's Proton does in `vrclient` so that Windows VR games run on Linux —
`load_compositor_texture_dxvk()` in
[Proton's vrclient_x64](https://github.com/GloriousEggroll/proton-ge-custom/blob/master/vrclient_x64/cppIVRCompositor_IVRCompositor_020.cpp)
— and Elite Dangerous VR under Proton runs through exactly that
translation. The approach has been field-tested against this game's
specific submit pattern (one double-wide texture, per-eye bounds) for
years; what has not existed is anyone doing it on native Windows, because
on native Windows nobody stands between the game and `openvr_api.dll`.

EDVR does. That is the whole observation: **the openvr proxy exists because
frame decisions have to be made where frames are handed to SteamVR, and
this is another decision of exactly that shape.** The translation is a
final step at the two places `hookedSubmit` actually forwards a texture
(the normal path, and the shadow-substitute path — both funnel through
`realSubmit`), downstream of every existing decision, so the withhold
logic, the pair latch, the shadow and the crop never learn anything
changed. The shadow and crop textures live on the same DXVK device, so the
same translation serves whichever handle a path ends with.

DXVK even prepares its own side: its
[OpenVR extension provider](https://github.com/doitsujin/dxvk/blob/master/src/dxvk/dxvk_openvr.cpp)
is active on Windows — it finds `openvr_api.dll` in the process, asks the
compositor which Vulkan instance and device extensions it requires, and
enables them on the device it creates, precisely so that a compositor can
consume its images. (Pleasingly, "asks the compositor" means calling
`VR_GetGenericInterface` on the loaded `openvr_api.dll` — which in an EDVR
install is EDVR's proxy, so the vr log would show DXVK's queries arriving.
`DXVK_NO_VR=1` is the escape hatch if that interaction ever misbehaves.)

### The shape of the code

House rules applied to a new module, `src/openvr/dxvk_submit.{h,cpp}`:

- **Detect, don't configure.** First validated Submit: QueryInterface the
  handle for `IDXGIVkInteropSurface`. Real D3D11 answers no — the module
  goes permanently quiet and costs one QI. DXVK answers yes — the bridge
  arms, and says so with the negotiated VkFormat and queue family, once.
- **Declare the ABI locally**, the `openvr_min.h` way: the two interop
  interfaces (their IIDs are published constants) and
  `VRVulkanTextureData_t`. No DXVK headers, no build-time dependency.
- **Validate before acting; stand down loudly.** `GetVulkanImageInfo`
  returning something that is not a sane 2D color image, a missing interop
  device, a failed transition — each falls back to forwarding the original
  D3D11 submission (broken VR, exactly as without the bridge) with a log
  line naming which check refused. Fault budget and the existing
  compositor sentinel cover the crash class.
- **Config**: `fix.dxvk_submit = auto` (default; does nothing off DXVK) /
  `off`. An `on` that forces the attempt exists only if a detection failure
  mode ever shows up in the field.
- **Perf posture**: per-submit cost is a flush, two layout transitions and
  a queue lock — the same bill Proton pays on every frame of every D3D11
  VR game it runs. Measured, not assumed, before any perf claim.

Order of magnitude: a few hundred lines plus tests, against the existing
`tools/fakevr` harness extended with a fake interop surface. Small next to
the verification it obligates.

## What only running it can settle

In this codebase's own idiom: these are the things the design *believes*
and has not *measured*, each with the failure it would produce.

1. **The runtime accepts `TextureType_Vulkan` through `IVRCompositor_014`.**
   Elite links a 2015-era interface; Vulkan texture types postdate it. The
   struct is version-independent and the runtime behind every interface
   version is one implementation, so this should be a non-event — but if
   the old marshalling path rejects the type, the fallback is EDVR
   fetching a current `IVRCompositor` from the real runtime for its own
   submissions, which is more machinery and wants its own validation.
   *Failure mode: Submit returns an error, bridge stands down, log says so.*
2. **The DXVK device carries the compositor-required extensions.** DXVK's
   provider queries the compositor at device creation — which only works if
   the game has initialised OpenVR by then. The ordering is observable for
   free in an existing install: does the vr log's first
   `VR_GetGenericInterface` line precede the gfx log's device-creation
   line? If the provider misses, external-memory extensions DXVK enables
   for its own sharing support may still suffice.
   *Failure mode: Submit fails on first frame; stand down.*
3. **Colour space survives the translation.** D3D11 submissions let the
   runtime infer gamma-vs-linear from the DXGI format; the Vulkan path
   names a VkFormat explicitly (from `GetVulkanImageInfo`) plus the game's
   own `EColorSpace`. A mismatch is not subtle — it is the exposure-fix
   test scene looking washed out or crushed — which makes it easy to
   verify and easy to catch.
4. **The interop ABI is what the published headers say** on the DXVK
   version actually shipped to users — pin one tested release (and decide
   2.7.1 vs 3.x per the Vulkan 1.4 driver floor) rather than tracking
   master.
5. **The whole matrix behaves**: NVIDIA is the only Windows driver
   upstream meaningfully tests; Arc wanted a config workaround; AMD on
   Windows is explicitly rough at the 3.0 boundary. Field verification on
   this project has meant Quest 3 via Virtual Desktop and Pimax via
   PiOpenXR — note that both of those are OpenComposite-family stacks, so
   the *SteamVR* bridge would ship with the same "real SteamVR is
   unmeasured, logs welcome" honesty the cull guard shipped with.
6. **The OpenComposite route is its own question.** Under
   `real_openvr_dll = opencomposite`, the D3D11 handles go to
   OpenComposite, which binds an OpenXR session on the game's (DXVK)
   device; the runtime then shares *its* swapchain images into that device
   — which is DXVK's *import* direction, the one the D3DKMT work made
   plausible on Windows. Plausible, unverified, and worth one experiment
   before designing anything for it: if it happens to work as of DXVK 3.0,
   the bridge's audience narrows to real-SteamVR users.

## Distribution, licence, support

DXVK is zlib-licensed, so bundling is legally trivial — and still the wrong
opening move. Version × driver churn says: don't ship DXVK, document one
pinned, tested release and link it, the way the README already treats
EDHM/ReShade as the user's own software that EDVR composes with. Bundling
becomes worth revisiting only if a bridge ships and one specific DXVK
version becomes load-bearing for it.

The real recurring cost is support surface. Every DXVK crash, driver
quirk, or shader bug in a chained install will arrive in this tracker
wearing EDVR's name — the same dynamic the EDHM chaining answer already
manages, but wider, because DXVK replaces the entire renderer underneath
both mods. The mitigations are the ones this project already practises:
the `forwarding to` line already names the chained DLL; add a DXVK
detection line (the interop QI answers it in one call) so triage reads it
in the first ten lines; and the README section states plainly which
combinations were verified and which are welcome-logs territory.

## Recommendation

Phased, each phase shippable alone:

- **Phase 0 — no code.** Verify the flat-screen chain on one rig
  (`real_dll = d3d11_dxvk.dll` + DXVK `dxgi.dll`), confirm hook modes,
  sentinels and totals lines look right, and write the README section with
  a plain "VR does not work this way, see below" marker. Settles unknowns
  2 (the ordering observation comes free) and 5's first data point.
- **Phase 1 — logging.** DXVK detection line in both logs; the
  `contextHookModeFor` note; a one-time warning when a validated Submit
  carries a DXVK texture and the bridge is absent/off — the line that
  turns "SteamVR shows nothing" bug reports into one-pass diagnoses.
- **Phase 2 — the bridge**, behind `fix.dxvk_submit = auto`, built and
  validated as above. This is the integration; everything before it is
  positioning.
- **Phase 3 — field verification**, the same way every fix here earned its
  README section: measured on real rigs, unknowns retired one log at a
  time, real-SteamVR reports solicited explicitly.

The case for doing it at all: Odyssey's frame pacing is the single most
VR-hostile thing about it that no in-scope EDVR fix touches, DXVK is the
one external tool that demonstrably moves it for some rigs, and EDVR
happens to already hold the exact hook that makes DXVK's one hard VR
problem solvable on Windows. Nobody else is positioned to do this. The
case against is the support surface, and the honest reading of this
project's rules is that Phase 2 does not start until Phase 0's log lines
and Phase 1's diagnoses exist to carry it.

# Upscaling and foveated rendering: a design

*A design document, written before the code. Claims about EDVR cite the
source; claims about runtimes, drivers and SDKs are labelled measured
(established in this repo's field logs or code), vendor-stated (their
documentation or release notes), or believed; what can only be settled at
implementation time or in a live session is collected under Phase 0.
Nothing here is implemented yet.*

## The ask

Port the performance features of OpenXR Toolkit into EDVR, so that
SteamVR/OpenVR players get them — including NVIDIA foveated rendering, and
an eye-tracked centre on headsets that can provide one.

OpenXR Toolkit is an OpenXR API layer, so for Elite it only ever worked
through OpenComposite: the game speaks OpenVR, OpenComposite translates to
OpenXR, and the toolkit sits inside that translation. Players on real
SteamVR — lighthouse headsets, Steam Link, Virtual Desktop's SteamVR mode —
were never able to use it, and the OpenVR-native alternatives that existed
(fholger's `openvr_fsr` and `vrperfkit`) are archived, and occupy the same
DLL slots EDVR occupies, so they cannot even be stacked alongside it. The
population this design serves is exactly the one nothing serves today.

The toolkit itself was discontinued in 2024. Its licence is MIT, so where
its code says something worth transcribing, it can be transcribed with its
notice — the same rule `src/d3d11/fsr/README.md` states for AMD's files.

## What translates, and what does not

The toolkit's performance page, feature by feature:

| Feature | Verdict |
|---|---|
| Upscaling (FSR) + sharpening | **Port.** Feature 1 below. |
| Sharpen-only (CAS mode) | **Port**, as a degenerate case of feature 1. |
| Fixed foveated rendering | **Port**, NVIDIA-only on D3D11. Feature 2. |
| Eye-tracked foveation | **Port**, via OpenVR's own new gaze API. Feature 3. |
| In-headset settings menu | **Port, reimagined.** Feature 4 below — the toolkit's one indispensable piece of UX, rebuilt for a cockpit. |
| Turbo mode | **No.** It defeats `xrWaitFrame` throttling, which is an OpenXR-runtime behaviour (WMR's, mostly). SteamVR paces through `WaitGetPoses`' running start and does not throttle that way; its own per-app settings already expose what is tunable. Re-implementing pacing is high regression risk for nothing measurable. |
| Frame-rate lock / motion-reprojection lock | **No.** The reprojection half is WMR-specific; SteamVR's motion smoothing already halves adaptively. |
| Metrics overlay | **No persistent overlay.** SteamVR ships a frame-timing overlay already. EDVR measures its **own** passes by timestamp query and reports them in the log — and, once the menu exists, live on its status page (feature 4). A HUD that stays up while you fly remains out of scope. |
| NIS (NVIDIA Image Scaling) | **No.** It exists in the toolkit as a preference alongside FSR; FSR is already vendored here and does the same job. A second upscaler is surface without capability. |

Two adjacent things that come up in every discussion of foveation, and why
neither is available to this game:

- **Quad-views rendering** (how DCS and MSFS do eye-tracked foveation) needs
  the *game* to render four views. It is an OpenXR concept with no OpenVR
  equivalent, and Elite will never implement it. This is exactly why the
  VRS route below — which needs nothing from the game — is the right one
  for an OpenVR title.
- **NVIDIA VRSS/VRSS2** (driver-level foveated supersampling) requires
  forward rendering with MSAA and a per-title driver whitelist
  (vendor-stated). Elite's renderer is deferred, which disqualifies it
  regardless of the whitelist.

## What EDVR already owns

The reason this port is tractable is that every interception point it needs
is already built, field-hardened, and in daily use:

- **The resolution answer.** `hookedGetRecommendedRenderTargetSize`
  (`src/openvr/system_hook.cpp`) already multiplies the size handed to the
  game — the cull guard's stage 1 inflates it, and the game demonstrably
  treats the change as an ordinary supersampling event and rebuilds its
  targets (measured; it is the mechanism the guard's two-stage go-live is
  built on). Render scale is the same lie with a factor below 1.
- **Submit-side substitution.** `hookedSubmit`
  (`src/openvr/compositor_hook.cpp`) already swaps the game's texture for
  an EDVR-rendered one on three paths (theater, eye heal, guard crop), and
  `applyCullGuard` shows the discipline: one lambda applied identically to
  every forwarding path, so no path can ship an untreated frame.
- **FSR, vendored and running.** `src/d3d11/fsr/` carries AMD's unmodified
  EASU and RCAS; `intro_upscale.cpp` computes their constants
  (`FsrEasuCon`, `FsrRcasCon`) and runs both as compute passes today, with
  the HLSL generated from the vendored files at build time.
- **The cross-DLL renderer pattern.** The openvr half calls renderers the
  d3d11 half exports (`edvrFssTheater`, `edvrFssHealLeft`), resolved by
  `GetProcAddress`, standing down loudly on a mismatched pair.
- **The eye-target classifier.** The vScreen census
  (`src/d3d11/vscreen.cpp`) hooks `OMSetRenderTargets`, `RSSetViewports`,
  `ClearState` and every draw variant, and resolves per bind whether the
  bound target is an eye texture (`targetIsEyeSized`, the `rtv0Eye` cache)
  — fed the *real* per-eye size from the submit side rather than guessing.
  This is the hard part of any VRS integration, and it already exists.
- **The projection truth.** The system hook captures each eye's real,
  asymmetric tangents, and the lied tangents when the guard is live. Both
  matter below: the fovea's position in the image is a function of exactly
  these numbers.
- **Single-threaded rendering, measured.** Submit and Present run on the
  same thread (field, 2026-08-15 — the gate note in `compositor_hook.cpp`),
  and `ExecuteCommandList` has never been seen on this game
  (`vscreen.cpp`), so touching the immediate context from the submit hook
  is safe and there is no deferred-context state tracking to build.
- **The choreography.** Two-stage go-live, live re-stage on a config
  change, per-headset gating by FOV signature, and the stand-down pattern
  (`systemHookGuardStandDown`) all transfer unchanged.

## Feature 1 — render scale, upscaled back by FSR

**What it is.** The game is told the headset wants `render_scale` × fewer
pixels per axis; it renders smaller, cheaper targets. At submit, EDVR runs
EASU to reconstruct the image at the session's native size, RCAS to
sharpen it, and forwards that — an ordinary full-bounds submission at the
size the runtime expected all along. The runtime never learns anything
changed; the compositor's own bilinear upscale (what you get from just
lowering the SteamVR slider) is replaced by edge-adaptive reconstruction
plus sharpening, which is the whole reason 75–85% scales stay presentable.

**Guidance the setting ships with:** keep the SteamVR resolution slider and
Elite's HMD Image Quality where you normally run them, and come down with
`render_scale` — the win over those sliders *is* the reconstruction, which
only happens when EDVR does the scaling.

**Mechanism, in the cull guard's own shape:**

1. Stage 1: the size answer shrinks by the factor. The game rebuilds its
   targets smaller — to it, the same event as the runtime moving the
   render resolution, which the session already handles (the
   `noteEyeTextureSize` machinery re-reads and re-publishes on exactly
   this).
2. Adoption: when both eyes submit at the shrunken size (the guard's
   changed-size test, reused), the upscale pass arms. Until then every
   frame forwards untouched — the transport never sees a shape the
   session has not served.
3. Live: at each submit, the d3d11 half's exported pass (working name
   `edvrUpscaleEye`) runs EASU from the game's texture into an EDVR-owned
   native-size texture, RCAS in place, and the submit forwards the result.
   Same export-and-stand-down contract as the theater.

**Sharpen-only mode falls out for free:** `render_scale = 1.0` with
`render_sharpness > 0` skips EASU and runs RCAS alone at native size —
the toolkit's CAS mode, at unchanged cost.

**Interactions, each decided here:**

- **Cull guard.** The two features are the same two hooks pointed in
  opposite directions, and they compose: the size factors multiply at the
  answer (guard wants margin pixels, scale removes visible-field pixels),
  and at submit the crop runs first, the upscale second. End state is a
  fused pass — EASU's constants take an input sub-viewport
  (`FsrEasuConOffset`, vendor-stated), so crop+upscale is one dispatch —
  but v1 may ship sequential (crop copy, then upscale) because the crop
  copy already exists and fusion has an edge-tap subtlety (Phase 0).
- **Transition flash.** The resubmit shadow keeps holding the *game's*
  texture — the full, small frame — and a withhold's handover goes
  through the upscale like any forward. Same reasoning as the guard: the
  shadow stores source, treatment is applied at the door, once, for every
  path.
- **Theater and heal.** Both already substitute EDVR-drawn textures built
  from game content; whatever texture a path is about to forward is what
  the upscale treats. One lambda, every path — the `applyCullGuard`
  discipline, extended.
- **The eye-size channel.** `noteEyeTextureSize` reads the texture the
  *game* submits, before any substitution, so the d3d11 half keeps
  matching the game's real (small) render targets with no change. The
  0.7.3 lesson — publish what is real, not what is guessed — already
  covers this feature.
- **An openvr-only install** (no EDVR d3d11.dll) has nobody to run the
  pass. The feature then refuses to shrink at all — a small render
  bilinearly upscaled by the compositor is a quality regression this
  project would be silently causing — and says so once, in the theater's
  "mismatched pair?" voice.

**Formats.** The pass refuses what `guardCropCopy` refuses (MSAA, arrays,
mips) and creates typed SRV/UAV views over the formats the field actually
submits; sRGB variants are a Phase 0 measurement, not an assumption.

**Settings sketch** (final names at implementation, `[fix]`):

```
render_scale     = 1.0    ; per-axis, 0.5–1.0; 1.0 = off. Restages live.
render_sharpness = 0.0    ; 0–1 RCAS strength; live. >0 alone = sharpen-only.
```

Pixel cost quoted in the log as factor²: `render_scale = 0.8` renders ~64%
of the pixels. The pass's own GPU price is measured by timestamp query and
printed alongside — believed a fraction of a millisecond per eye at
headset sizes, from the intro upscaler's behaviour; measured properly in
Phase 0.

## Feature 2 — fixed foveation, by variable-rate shading

**The hardware truth first, stated plainly:** Elite is D3D11, and on D3D11
variable-rate shading is only reachable through NVAPI, which means
**NVIDIA Turing or newer** (RTX 20-series / GTX 16-series up). AMD and
Intel expose VRS only in D3D12; there is no D3D11 path for them to expose
(vendor-stated, and it is the same limitation OpenXR Toolkit has on D3D11
titles). This feature helps a large slice of the player base and is
honest about which slice.

**What it is.** A shading-rate image — one byte per 16×16-pixel tile
(vendor-stated Turing granularity, confirmed at init) — divides each eye
into rings: full rate in the centre, one shade per 2×2 pixels further
out, one per 4×4 at the edge. The pixels still exist at full resolution;
they are shaded in coarser groups, which is where deferred lighting spends
its time. Field experience with vrperfkit's identical mechanism in this
game: roughly 10–30% GPU depending on preset and scene, planetary
surfaces the big winner. It does nothing for CPU-bound settlement frames,
and the docs will say so.

**Where it hooks.** In the `OMSetRenderTargets` thunk the census already
owns: when the bound target classifies as an eye-scene target
(`targetIsEyeSized` — the existing verdict, not a new heuristic), set the
shading-rate view and per-viewport rates
(`NvAPI_D3D11_RSSetShadingRateResourceView`,
`NvAPI_D3D11_RSSetViewportsPixelShadingRates`, vendor-stated names);
when anything else binds, clear them. The `ClearState` thunk clears
EDVR's record of what is set. No deferred contexts to chase (measured,
above).

**The fovea is not the texture centre.** In an asymmetric frustum the
straight-ahead point sits at tangent (0,0), which lands off-centre in the
image — on a Quest 3 the horizontal frustum is 54°/40° (measured, the
terrain investigation), so centring the rings on the texture would put
full resolution in the wrong place and coarse tiles at the fovea. The
rings are computed from each eye's own tangents, which the system hook
already captures — and from the *lied* tangents when the guard is live,
because the mask lives in rendered-image space.

**The guard synergy.** When the cull guard is live, its overscan margin is
rendered only to satisfy the game's culler and cropped before submission —
no one ever sees those pixels. The mask shades that whole band at the
coarsest rate whenever the guard is live, reclaiming most of the guard's
measured ~6% even with foveation otherwise off. Not rate-zero (the rates
include a cull), deliberately: post passes sample neighbourhoods, and a
black margin would bleed into the visible edge through bloom; a coarsely
shaded margin holds plausible content. The band comes from
`cropFractions`, which already exists.

**Risks, and their shape.** Coarse shading of deferred lighting and post
shows as peripheral shimmer or blocky bloom — visible and tunable, never
corrupting. Mitigations in order: conservative default rings; rates per
preset; and if the field names a specific pass that gets mauled, the
census can identify its draws for exclusion the same way it identifies
everything else. This is the feature's whole risk budget: there is no
failure mode here that outlives a settings change or a restart.

**Fail-safes.** Arming requires NVAPI to initialise, the rate view to
create, and the first set call to return OK; any other answer is one log
line — "foveation: unsupported here (this needs an NVIDIA RTX/GTX-16 GPU
or newer)" — and off. An NVAPI error after arming clears all VRS state
and stands down loudly for the session, the guard's pattern. NVAPI is
calls into the driver: nothing here touches game code or memory, and the
headers are vendored under NVIDIA's redistribution licence with the
licence text travelling alongside, as `fsr/README.md` does for AMD.

**Settings sketch** (`[fix]`):

```
foveation          = off   ; off | quality | balanced | performance | custom
foveation_headsets =       ; same FOV-signature gate as cull_guard_headsets
; custom: ring boundaries in degrees of visual angle, per-ring rates
```

## Feature 3 — the eye-tracked centre

**What changed since the toolkit era.** The toolkit needed a per-vendor
SDK for every eye tracker (Varjo, SRanipal, Omnicept, Droolon). Valve has
since built gaze into OpenVR itself (vendor-stated, the OpenVR SDK
release notes): from SteamVR 2.8.3 the OpenXR side advertises
`XR_EXT_eye_gaze_interaction`; from SDK 2.12.14 any third-party driver
can publish eye data (`Prop_SupportsXrEyeGazeInteraction_Bool`,
`CreateEyeTrackingComponent` / `UpdateEyeTrackingComponent`); and the
application side of current `IVRSystem` carries
`GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame` and
`GetEyeTrackedFoveationCenter[ForProjection]` — the last two existing
*explicitly for foveation*. One vendor-neutral call, for any headset
whose driver publishes gaze: the Bigscreen Beyond 2e (whose foveation
work is being done with Valve directly), the Pimax Crystal's Tobii
tracker (a SteamVR driver shim demonstrating the path already exists),
and whatever adopts the driver API next. No SRanipal, no Tobii SDK, no
per-vendor adapters — which is also why per-vendor adapters are rejected
here rather than deferred: they are the thing this API exists to end.

**The rule this bends, bent in the open.** Elite links `IVRSystem_012`
(measured, the exe's interface literals), which predates gaze by a
decade; the methods live on modern interface versions EDVR would have to
request for itself. This project has a standing ban — the 6c rule,
`system_hook.cpp`: *IVRSystem is never called from inside the game* —
earned when a call into the 012 interface with a guessed ABI corrupted
the stack after appearing to work. This design asks for the first
deliberate exception, and the differences are the argument: the ban's
incident guessed the ABI of an *ancient* version from a modern header;
this requests a modern version by its own exact name, with that version's
own declarations, on EDVR's thread at the frame boundary, validated on
first call (a gaze ray has a shape: finite, unit-ish direction, origin
near the eyes — anything else means the pin is wrong, and the feature
goes inert loudly, never to guess). The exception is scoped to exactly
these calls; the ban stands everywhere else. If the maintainer prefers
the ban absolute, this feature waits — features 1 and 2 do not depend on
it, and fixed-centre foveation is most of the win.

**Where it cannot work, by construction.** OpenComposite-family stacks
implement the interface versions games use and will not serve a modern
`IVRSystem` (believed; confirmed or refuted in one log line at Phase 0) —
and players on those stacks already have OpenXR Toolkit and quad-views.
The launch centre already distinguishes `Runtime::Valve` from
`Runtime::OpenComposite` by export shape (`launch_centre.cpp`), so the
gaze path arms only where it can exist. The audience split is clean: this
feature serves real SteamVR, which is precisely where the new API lives.

**Mechanics.** Gaze is sampled once per frame at the `WaitGetPoses`
boundary (the hook exists; it is the same "before the game queries" point
every other per-frame decision uses). The ray maps to a per-eye image
point through the tangents in use — true ones, or lied ones under the
guard — and the mask regenerates when the centre moves a tile or more.
The mask is ~180×120 bytes at Quest-3-class sizes; regenerating it is
noise. Fallback is per-frame and silent: gaze invalid this frame (a
blink, a dropout) → fixed-centre rings; tracker gone → fixed rings until
it returns. A blink must not produce a log line.

**Privacy, stated the Explorer Cam way, because it is the same promise:**
a direction is consumed each frame, compared against the last mask
centre, and discarded. Nothing is logged, stored, or leaves the process;
the log says gaze is in use once, at arming, and nothing afterwards.

**Verification posture.** Neither field rig has an eye tracker, so this
ships the way the cull guard shipped for real SteamVR: implemented,
guarded, and asking for logs — the arming line, the first-call
validation verdict, and the mask-update cadence line are the whole ask.
The eye-tracked centre is a small delta on feature 2 (the mask generator
gains a moving centre); the risk lives almost entirely in the interface
pin, which is why the pin validates before it acts.

## Feature 4 — the in-headset menu

**Why it exists.** Every knob above is a taste-and-cost trade that can only
be judged with the headset on, mid-flight, over the scene that is actually
struggling. Today that judgement runs through alt-tabbing to a text editor
or the installer — workable at a desk, miserable over a planet. The
toolkit's menu, for all its looks, was the reason its features got tuned
at all. This is that idea, built the way this project builds things.

**What the toolkit's menu got wrong, which is the brief for this one:**
head-locked text that rode every head movement; pixel-fixed type that came
out tiny on wide-FOV headsets; three chorded function keys to navigate a
tree; and no sense of what a setting *cost* beyond watching the corner FPS
counter wobble.

**The shape of the fix, in five decisions:**

1. **World-anchored, not head-locked.** The panel spawns where you are
   looking, at a comfortable distance (~1.4 m, curved gently toward you —
   the `panel_curve` math, reused), and then stays put in the world while
   you read it: the theater's frozen-pose anchoring (`theaterXform`,
   reprojection-correct from inside Submit) is exactly this machinery.
   Re-summoning recentres it to wherever you look now. Fade in and out,
   ~150 ms; nothing else animates — motion on a panel is what looks cheap
   in a headset, not the absence of it.
2. **Type sized in degrees, not pixels.** Row text is specified as visual
   angle (minimum cap height ~1.1°, measured for comfort in Phase 0) and
   derived per headset from the tangents the system hook already captures
   — the same numbers that place the RemLok lines. A Pimax and a Quest 3
   get the same apparent size from different pixel counts. And the panel
   is composited *after* the guard crop and the upscale, onto the
   native-size outgoing frame, so its text is always native-crisp no
   matter how far `render_scale` is pushed — the menu never degrades with
   the setting it is adjusting.
3. **Look at a row; press one key.** Head-aim replaces up/down navigation
   entirely: the row under your gaze highlights (generous hitboxes,
   sticky hysteresis so head jitter never flickers it), and EDVR's own
   menu key activates — toggles toggle, enums cycle, sliders step, hold
   to repeat, with a chorded reverse for the other direction. When
   feature 3's gaze is live, the aim ray is your eyes instead of your
   head, and an optional dwell-to-select needs no key at all — the menu
   doubles as the eye tracker's live sanity check, since the highlight
   *is* where EDVR thinks you are looking. XInput pads navigate too
   (d-pad and face button — `xinput_watch` already polls pads
   edge-triggered for the FSS bindings); a HOTAS stays out of reach, as
   it already is, and head-aim is why that costs nothing.
4. **Keys that are checked, because they cannot be captured.** EDVR
   watches input and never blocks or injects it (`hotkey.h`'s polled,
   foreground-gated design — the Explorer Cam promise, unchanged), so any
   key the menu uses also reaches Elite. That is a fact to design around,
   not hide: the menu's keys default to chords Elite's stock bindings
   never use, and — the touch no other tool has — EDVR reads the
   player's *actual* bindings (`elite_binds`) and verifies the menu keys
   against them, warning in the log and on the panel itself when a menu
   key would double-act in the ship. Head-aim keeps the key count to
   two.
5. **Every setting wears its price.** Each row shows what its current
   value costs or saves, measured, not estimated: `render_scale` shows
   the percentage of pixels being rendered and the upscaler's own
   timestamp-query milliseconds beside it; foveation shows the rings'
   savings; the status page shows the frame time and every EDVR pass's
   share of it. The project's rule that every feature ships with its
   price measured, promoted from the log into the interface. This —
   more than any styling — is the "user friendly" the toolkit lacked:
   the player watches the trade they are making, live, over the scene
   they are making it for.

**What it shows.** Two pages at first, deliberately few rows each:

- **Performance** — `render_scale`, `render_sharpness`, `foveation`
  preset, the gaze centre on/off with a live tracking indicator. Each row
  carries a one-line plain-language hint (the ini's own explanations,
  shortened; `edvr.ini` remains the deep documentation) and a
  takes-effect-at-restart badge where that is true, exactly as the
  installer's screen says it.
- **Status** — the README's "checking it worked" section as a live panel:
  which runtime is underneath (the launch centre's verdict), the real eye
  size, guard state, scale state, whether VRS armed and if not why not,
  gaze availability, frame time with EDVR's passes itemised. Read-only,
  and the page a support thread will ask to see.

The row registry is data-driven off the same config keys, so later
live-tunable settings (panel distance, sun glare mode, the particle fix)
can join without new machinery — but the menu ships scoped to what this
design adds, plus status.

**Toasts, the menu's little sibling.** When a live setting changes — from
the menu, the installer, or a hand edit to the ini — a one-line
confirmation fades through the lower view for a couple of seconds:
`render scale 0.80 — rendering 64% of the pixels, upscaler 0.21 ms`.
Today that feedback exists only in the log; a player tuning by ini edit
deserves to see the change land without taking the headset off.
`menu_toasts = off` for players who want nothing uncommanded on screen,
ever.

**How it is built.** The panel is rasterised on the CPU — DirectWrite for
the glyphs, into a system-memory bitmap, uploaded once per *change*, not
per frame — so there is no font engine on the GPU and no per-frame text
cost; per frame the menu is one textured, alpha-blended quad per eye. The
split follows the theater exactly: the openvr half owns the decision to
show, the anchor pose and the input; the d3d11 half owns rasterisation
and the draw, behind one export (`edvrMenuPanel`, working name), standing
down loudly on a mismatched pair. While the menu or a toast is up, frames
route through an EDVR-owned copy (the shadow path that already exists) so
the game's own textures are never drawn on; when nothing is shown, the
whole feature is one hotkey poll per frame.

**Persistence, with one source of truth.** The menu writes `edvr.ini` —
surgical value edits that preserve every comment, by the same config
grammar `config.cpp` reads and the installer's merge already parses
(`iniedit`'s contract: a merge that disagreed with the reader would write
a file whose settings are not the ones it reports; the same holds for a
menu). Changes apply in memory immediately and the file carries them to
the next session; the installer's settings screen and the menu can never
disagree, because neither owns any state the other lacks. The desktop
screen remains the place for everything; the menu is the flight-relevant
subset.

**Safeguards, because they are the reason to trust it:** it never appears
uncommanded (toasts are the one exception, and they have an off switch);
it auto-dismisses after idle seconds; it draws only into EDVR's own
copies, never the game's textures; its keys are watched, never taken —
the game receives every press, which is why the collision check exists;
and a rasterisation or draw failure means no menu and one log line, with
the game unaffected. Closed, it costs one key poll; open, it costs the
copy path and says so on its own status page.

**Settings sketch** (`[hotkey]` and a new `[menu]` section):

```
[hotkey]
menu            = CTRL+ALT+E   ; summon/dismiss; verified against your binds
menu_adjust     = SPACE        ; activate/step the aimed row (chord reverses)

[menu]
distance        = 1.4          ; metres; live
idle_dismiss    = 20           ; seconds; 0 = stay until dismissed
menu_toasts     = on           ; the transient change confirmations
```

Defaults chosen at implementation after a sweep of Elite's stock binding
sets; the collision check, not the choice, is the real safety.

## Phase 0 — what must be measured, not assumed

One instrumented session (plus desk checks against SDK headers) before or
during implementation, in the head-steer convention:

1. **Eye-texture formats in the field.** The exact DXGI formats (sRGB?
   typeless?) Elite submits on both rigs, for the EASU/RCAS views. The
   pass refuses anything unmeasured.
2. **EASU/RCAS edge taps under the fused crop.** RCAS reads a 3×3
   neighbourhood, EASU a 12-tap ring; at the crop's edge those taps must
   clamp inside the region or the margin bleeds in. Decides fused vs
   sequential for v1.
3. **The upscale pass's price.** Timestamp queries, both rigs, quoted in
   the log the way the guard quotes its margins.
4. **NVAPI caps on the target GPUs.** Tile size (16 assumed), available
   rates, and behaviour on hybrid-GPU laptops (believed fine; NVAPI
   resolves the render GPU, but believed is not measured).
5. **Which passes tolerate coarse rates.** The artifact walk: presets
   against stars, bloom, smoke, planet rims. The census names any draw
   that needs excluding.
6. **The gaze interface pin.** The exact modern `IVRSystem_0xx` version
   string to request (from the SDK header at implementation), that
   SteamVR serves it alongside the game's 012 in one process (believed,
   from how vrclient versions interfaces; one log line settles it), and
   the first-call shape validation against a real tracker.
7. **Interplay flights.** Guard + scale composed; a withhold under scale;
   the theater under scale; a SteamVR resolution change *while* scale is
   live (the size channel should follow; watch it do so).
8. **DirectWrite beside the game.** CPU rasterisation into a
   system-memory bitmap in the game's process is believed inert; measure
   that it is (no device interop, no per-frame cost, no loader surprises
   at first use mid-session).
9. **Legibility, in degrees.** The minimum comfortable cap height and row
   spacing at 1.4 m on both rigs — the number the panel's whole layout is
   derived from.
10. **Head-aim comfort.** Hitbox size and highlight hysteresis that a
    resting head does not flicker and a deliberate glance lands first
    time; measured with the panel up, not reasoned about.
11. **The collision sweep.** Menu-key defaults checked against Elite's
    stock keyboard and pad binding sets, and the live check exercised
    against a real player's binds file.

## Phasing

1. **Render scale + sharpening** — all vendors, biggest reach, assembles
   parts that already exist. Ships first and alone, ini-tuned.
2. **The menu** — driving phase 1's two knobs plus the status page.
   Deliberately second: it multiplies the tuning velocity of everything
   after it, and every later phase lands as one more row instead of one
   more reason to alt-tab. Toasts ride along.
3. **Fixed foveation (VRS)** — NVIDIA-only, rides the census classifier;
   conservative presets; the guard-margin synergy; a row and a status
   line in the menu.
4. **Eye-tracked centre** — the moving centre on 3's mask, gated on real
   SteamVR, a driver that answers, and the 6c exception holding up. The
   menu gains gaze aim and dwell, and its highlight becomes the live
   check that the tracker and the mapping agree.

Each phase off by default, each with its own stand-down, each logging its
price. Nothing in any phase reads or writes game memory or code: render
scale and the gaze read edit answers and frames from outside (the guard's
posture exactly), foveation adds calls into NVIDIA's driver, and the menu
draws only on EDVR's own copies and watches keys it never takes. A player
who wants none of it leaves the settings at their defaults and runs a
build identical in behaviour to today's.

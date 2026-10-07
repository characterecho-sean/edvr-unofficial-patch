# Plugins: every fix a selectable module (design, 2026-09-30)

## Status

- **State (2026-10-07):** implementation in progress on
  `codex/plugin-architecture`; do not merge to main until Sean is ready to
  ship. Phase 1 of five is in progress; review and remaining gates: section 11.
- **Goal (Sean):** every fix and performance item belongs to one plugin,
  plugins group features logically, and the user picks which to install in the
  installer. A plugin that is not installed costs nothing.
- **Built on:** a read-only inventory of main `4296f142` (25 features, 225 ini
  keys, 19 draw verdicts, the services they share; section 2), the flat CPU
  census of 2026-09-30 (hook entry about 1.4 ms over 17,180 calls a frame on
  foot; D3D call counts are what a wrapper such as ReShade multiplies), the
  landing-pad regression of 2026-09-29 (a claim order nobody had written down)
  and PR #46's diff.
- **North star (Sean, 2026-09-30): performance.** The plugin layer never makes
  EDVR slower, and its first phase makes it faster. Gates are relative: each
  phase flies the same spots and must be no worse than the previous phase,
  within noise (section 7).
- **Decided, Q1 (Sean, 2026-09-30): monolithic.** First-party plugins are
  modules inside the one DLL: each a static library behind the plugin
  interface, a build gate against one plugin including another's internals, and
  an unselected plugin never registers. The interface stays C-compatible so a
  split into DLLs stays mechanical (section 10 has the reasons). PR #46's C ABI
  stays the add-on tier, moved into the core so flat mode gets it too.
- **Decided, Q2-Q6 (Sean, 2026-09-30):** the nine plugins of section 4
  (regrouped: intro and on-foot-panel split out, UI quality inside
  temporal-aa); defaults that reproduce today's shipped behaviour; the add-on
  tier after Phase 1 (PR #46's OM-unbind fix lands on its own now); diagnostics
  probes out of the default install, census kept in the core; today's keys and
  sections kept, each owned by a plugin. Section 10.
- **Decided, Q7 (Sean, 2026-09-30): graphics-only VR is its own phase, after
  Phase 1.** When no selected plugin needs the OpenXR runtime (section 4.1),
  the installer skips it and Elite stays on its stock VR path. Four changes
  first (section 10); no F8, AA, flash fix or Explorer Cam without the runtime;
  the first build needs a flight on a stock runtime.
- **Next:** main integrates through `6b43ffc9` (code unchanged from
  `0d4bc714`). The NV pilot's legacy replay, 32 composed children and pixel
  coverage run. Whole-ladder actions precede broader migration. The refreshed
  814 caller is diagnostic: its body gains 52 instruction bytes/14 records, raw
  equality fails and timing remains unmeasured. New cold census
  sample/calibration rows and the strict `edvr_log.py --plugin-cost` reader
  pass the full 149-job build and installer checks, 344 focused census checks
  and 17 fresh scoped gates. The complete main-based control `405b14cd` passes
  129 jobs with identical census files. Frozen flights: `7bbe7d90`
  control/`6c63f6aa` candidate, build/environment matched; no visual changes.
  NV-on sampled hook time per timed draw is 15.0% lower; other CPU ranges
  overlap. Carrier DLSS/NV-off EDVR GPU cost is 2.9% higher, its median above
  the control range. Draw counts differ and the on-foot source has 4.175% more
  pixels; normalization cannot establish GPU attribution/non-regression.
  Domain/frame/lifecycle accounting, held-boundary frame cost and full coverage
  remain open. Replay retains 482.4 MiB when enabled, none when off; NV blur
  reproduces on `14a7ff70`. Temporary key: `advanced.draw_replay` (off);
  removal requires Scope control. V2 preserves V1/CPU limits. No Phase 1
  acceptance or shipping.
- **Ruled out while designing:** loading every DLL found in a folder (DLL
  planting; the installer's receipts already know what it installed), a stable
  ABI for first-party plugins (they ship with the core; freezing their
  interface buys nothing and costs every refactor), and a per-draw virtual call
  into every plugin (section 7's measured costs forbid it).

## 1. Goals and non-goals

Goals:

1. Every fix is owned by exactly one plugin. A plugin groups features that
   share code, state or a purpose, and says so in a manifest.
2. The installer offers plugins per profile (VR, flat) with descriptions,
   dependencies and a recommended set. Not installed means: no hooks for it,
   no per-draw work, no menu rows, no ini keys, no log lines.
3. Plugins talk to the core through one declared interface. Contested draws
   are arbitrated by a declared precedence table that a rig replays.
4. Every log names the plugin set that ran, so a flight stays evidence.
5. Third parties can add overlays and panels (PR #46's MFD case) through a
   narrower, stable ABI.

Non-goals: a stable ABI for first-party plugins; loading or unloading
plugins while the game runs (install-time choice, plus the live on/off keys
there are today); any change in a fix's behaviour during the migration.

## 2. What exists today

Features by area (full table in the inventory report; switch key and
default first):

| Area | Features | Profile |
|---|---|---|
| Temporal | temporal AA (TAA/DLSS/FSR) with engine motion, camera jitter, screen/weapon motion (the celestial/terrain hook retired 2026-10-01); sharpening (RCAS); UI, smoke and hologram depth; the flat adapter | VF |
| Interface | UI quality (layer, panel scale, hologram remaps) | V |
| Cockpit visuals | sun glare, particle billboards and witchspace stars, RemLok lines, wake pulse, target indicator, night vision | V |
| Light | exposure share and damping | V |
| Scanners | FSS eye sync (heal, reveal, panel, res), scanner body | V |
| Intro and loading | intro video, splash backdrop, loading dim, loading hologram | V |
| On-foot panel | black void, screen resolution, panel distance, curvature, weapon stability | V |
| Comfort | transition flash, Explorer Cam | V |
| Performance | cull guard, FOV trim, OpenXR resolution, settlement detail, static props | V |
| Always | F8 menu and FPS overlay, input gate, diagnostics (census, dumps, probes) | VF |

How it hangs together now:

- **Hooks are process-lifetime and shared.** Device, context, swap chain,
  factory, DirectInput vtables; IAT hooks (input, intro skip); code hooks
  through the kinematic relay; memory patches. A disabled feature keeps its
  hooks; only `advanced.d3d11_fixes = 0` or a sentinel trip removes them.
- **The draw ladder.** `beginPanelOverride` (vscreen.cpp) returns one of 19
  `DrawVerdict`s; the first claim wins. A new verdict touches four lists (the
  enum, `forwardVerdictBegin/End`, `alteredFixOf`, `uiLayerVerdictForwards`).
  Non-verdict features compose in `forwardWithVerdict` (UI and hologram
  depth flags, engine-motion substitution, the UI layer). The defaults keep
  the draw gate open, so every eye draw walks the ladder with shape-first
  predicates whether or not the features are on.
- **Contested draws are settled by position.** The sun glare, the radar's
  contact family and the landing pad share one shader pair (VS
  94D5C556DFD6D705 / PS 912477AEF6958379); glare wins because it comes first.
  The 2026-09-29 pad attempt changed that arbitration and broke the glare.
- **Configuration.** 225 keys, all read and documented (the contract gate);
  33 configure calls at install and 35 on reload, 28 gate terms, a shutdown
  list, 51 frame-boundary ticks a Present. The architecture review
  (reviews\architecture-review-2026-09-29.md, T1) calls the missing feature
  registry the main debt.
- **Installer.** The edition is fixed by the artifact (VR or flat
  installer); `components` is a write-only string; there is no component
  choice. F8 rows are generated from the ini's `# ui:` lines.
- **No rig** covers exposure, sun glare, particles, RemLok, the loading
  hologram, wake pulse, target indicator, black void, the on-foot screen,
  the intro, backdrop and loader panels, or the FSS panel family.

PR #46 adds `include\edvr_plugin_api.h` (API v2): `EdvrPluginRegister`,
`onInitialize/onShutdown/onUpdate/onRenderEye/onFilterInput`, and host
services `registerSetting` (an F8 "Plugins" tab) and `logNote`. The manager
lives in the OpenXR runtime and loads every DLL under `plugins\`. What it can
express: overlays drawn over the finished eye, with the pose, keyboard
capture and F8 toggles. What it cannot: any draw-level fix, tees, code hooks,
temporal AA, the UI layer, engine motion, pose or projection edits, and all
of flat mode (no XR renderer, so no manager). Defects found in the diff:

- two `PluginManager` singletons, of which only the runtime's is
  initialised, so the menu's input filter never sees a plugin;
- the runtime never opens EDVR's log, so plugin lines are dropped;
- no fault containment around callbacks;
- unsigned DLLs loaded from a folder;
- `dt` fixed at 16 ms.

The OM-unbind fix in the same PR is unrelated and should land on its own.

## 3. Three tiers

- **Core** (always installed, one per profile as today): every hook, the
  draw dispatch, and the services in 3.1. It has no fixes of its own.
- **First-party plugins:** EDVR's fixes, grouped as in section 4, built into
  the one DLL as modules (Q1): each a static library behind the plugin
  interface, with a build gate that fails when one plugin includes another's
  internals. A plugin the install did not select never runs its
  registration.
- **Add-ons:** third-party DLLs through a stable, versioned C ABI (section 6)
  with narrow extension points: overlays, panels, input, settings.

### 3.1 Services the core owns

Hooks (vtable, IAT, code, patches); draw dispatch and claim arbitration;
binding shadows and the shader registry (fnv1a64 at creation); config with
per-plugin key ownership and the contract gate; logging with a plugin prefix;
fault containment (a `FaultBudget` per plugin, the sentinel naming the
plugin); the GPU and CPU census with families grouped per plugin; the draw
census and eye dumps; the F8 menu built from plugin schemas; the input gate,
hotkeys and game bindings; the journal watch; the profile descriptor; frame
ticks; the OpenXR runtime host on the VR side; the install manifest.

Engine motion, camera jitter and the temporal backends serve only temporal
AA, so they stay inside that plugin rather than becoming services. UI
quality ships inside it too (Sean, 2026-09-30), so the UI layer's use of the
temporal pass's frame state never crosses a plugin boundary.

## 4. The plugins

| Plugin | Contents | Profiles | Needs | Default VR / flat |
|---|---|---|---|---|
| temporal-aa | TAA, DLSS, FSR, engine motion, camera jitter (VR frustum, flat camera path), screen and weapon motion (the celestial/terrain hook retired 2026-10-01), UI/smoke/hologram depth, UI quality (the layer, panel scale, hologram remaps; VR), sharpening, the flat adapter (stand-down, F8 warning) | VF | NGX DLL for DLSS | installed, mode off, UI quality 100 / installed |
| cockpit-visuals | sun glare, particles and witchspace stars, RemLok, wake pulse, target indicator, night vision | V | - | on / - |
| exposure | exposure share and damping | V | - | on / - |
| scanners | FSS eye sync family, scanner body | V | - | on / - |
| intro | intro video, splash backdrop, loading dim, loading hologram (`holo_pattern`) | V | - | on / - |
| on-foot-panel | black void, screen resolution, panel distance, curvature, weapon stability | V | temporal-aa for weapon stability's motion half (soft) | on / - |
| comfort | transition flash, Explorer Cam | V | - | on / - |
| performance | cull guard, FOV trim, OpenXR resolution, settlement detail, static props | V | - | installed, off / - |
| diagnostics | probes, eye dumps, developer instruments | VF | - | off / off |

The per-frame census lines (GPU census, flat CPU census, LONG FRAME) stay in
the core: user logs are how field reports get diagnosed, so they are never
optional. Dependencies are hard (the plugin cannot load without) or soft (a
feature inside degrades and says so); the manifest records which.

temporal-aa is one plugin on purpose: its parts share per-draw state, the
camera and the history, and splitting them would put that state on a plugin
boundary. UI quality ships only with it (Sean): the layer exists to keep the
interface crisp under temporal AA and shares its frame state; its panel-size
half keeps running with the AA mode off, as today. It carries two adapters:
the per-eye VR one, and the single-image
one that serves flat mode today and could serve the VR on-foot screen and the
HDR route (docs\design-flat-temporal-aa-2026-09-23.md, section 81).

### 4.1 What needs the OpenXR runtime

D = works with only d3d11.dll installed, on a stock runtime; G = works
without the runtime, degraded; R = needs it. A read-only check of main
4a4f7fa2. The graphics half never calls openvr_api.dll, and every value the
runtime publishes reads "no answer" when absent, so the rows are classes of
behaviour, not crashes.

| Plugin | Without the runtime | Evidence |
|---|---|---|
| temporal-aa | R: TAA, DLSS, FSR, jitter, sharpening and UI quality run at the runtime's Submit (treatEye, the door) | native_temporal.cpp:282, 380 |
| cockpit-visuals | D particles, witchspace, wake pulse; G sun glare, night vision, target indicator, RemLok (eye-branch fixes need eye recognition) | vscreen.cpp:1976-2001, 2060 |
| exposure | D | exposure_fix.cpp:735-940 |
| scanners | G scanner body; R FSS eye sync (the heal is a runtime door, and the body-layer stamp reads the published eye size) | native_fss.cpp:120; vscreen.cpp:2275 |
| intro | D skip, backdrop blit; G movie, loader panel, scrim; R splash dim, loading hologram | intro_skip.cpp; splash_dim.cpp:113-119; holo_fix.cpp:159 |
| on-foot-panel | G black void, panel distance, curvature; D fixed resolution, R `auto`; R weapon stability | vscreen.cpp:2843; vscreen_res.cpp:358; native_frame.cpp:404-413 |
| comfort | R: the flash withhold and Explorer Cam's offset happen in the runtime | native_frame.cpp:497-509; head_offset_gate.cpp:1046-1061 |
| performance | R cull guard, FOV trim, OpenXR resolution; D static props, settlement detail `reduced`; G `auto` (holds at 1.0 without timing) | native_cull_guard.h; lod_governor.cpp:1273-1279 |
| diagnostics | D census and probes; R eye dumps | temporal_pass.h:90-98 |
| core: F8 menu | R: no door, and the anchor pose comes from the runtime | menu.cpp:3222-3232; native_menu.cpp:77 |

Eye recognition is the hinge. The eye size reaches the graphics half only
from the runtime (eye 0's treat, native_temporal.cpp:317). Without it
`targetIsEyeSized` guesses 2048 or more on both axes (vscreen.cpp:1253), so a
headset with a smaller eye axis gets no recognised eye draws and every
eye-branch fix goes inert, silently. `advanced.eye_render_size` pins it by
hand.

Install components. Graphics (d3d11.dll) for every plugin. The runtime set
(`Openvr\win64\openvr_api.dll` with the game's original renamed aside,
`openxr_loader.dll`, its license, `edvr_openxr.ini`) for temporal-aa,
comfort, scanners' FSS sync, and performance's cull guard, FOV trim and
resolution; intro and on-foot-panel use it only for their R rows. The NGX DLL
(`nvngx_dlss.dll`) for DLSS only. Profile and ini always. Flat already
installs as graphics + profile + ini (+ NGX) with no runtime (plan.cpp:314,
571; install_edvr.py:1186-1189).

## 5. The plugin contract

- **Manifest** (compiled in, and exported as JSON for the installer): id,
  name, description, profiles, requires and conflicts (hard or soft),
  default per profile, the keys it owns, its hook points, its claims, a cost
  note from the census, its rigs.
- **Lifecycle:** init(host) with a services table; configure(cfg), live as
  today; device and swap-chain creation; resize; device loss; shutdown.
- **Hook points:** per-draw classify and claim; claimed-draw begin and end;
  observers of Map/Unmap/Update, state setters, dispatch, clear and copy;
  Present before and after; frame-boundary ticks; runtime events on the VR
  side (eye submit, compose, pose); input; menu actions.
- **Claims:** each plugin declares the families it may claim (shader pairs
  or shape predicates) and a precedence. The ladder becomes a data table in
  the core. Overlapping declarations fail the build unless the table carries
  an explicit resolution entry. A rig replays recorded draw censuses through
  the table and requires the verdicts to match today's byte for byte (the
  rig the pad attempt lacked).
- **Interplay:** plugins exchange data only through core services, never by
  calling each other.

## 6. The add-on tier (PR #46 reworked)

Keep: the structSize-versioned C ABI, `EdvrPluginRegister`, `registerSetting`
and `logNote`, `onRenderEye` for overlays, `onFilterInput`. Change:

1. The manager moves into the core. It becomes one instance serving both
   profiles, so flat mode gets add-ons too. The flat overlay point is the
   final image before Present.
2. Load only add-ons named in the install manifest with matching SHA-256.
3. Wrap every callback in fault containment. A faulting add-on is disabled
   for the session and named in the log.
4. Log through `edvr::Log`.
5. Pass the real frame time.
6. Back add-on settings by the ini (an `[addon.<id>]` section), so they
   survive restarts and appear in the log bundle.
7. Versioning policy: the host supports API N and N-1.

## 7. Performance: the north star

Sean, 2026-09-30: EDVR already runs close to its budget, in VR above all,
so performance decides every trade-off in this design. The plugin layer
never makes EDVR slower, and its first phase makes it faster.

Measured on 2026-09-30 (flight 090706, on foot): hook entry costs about
1.4 ms over 17,180 calls a frame, and each D3D call EDVR makes is multiplied
by a context wrapper such as ReShade (one user's frame rate came back only
when ReShade was removed). The rules:

1. A plugin that is not installed, or installed but switched off, registers
   nothing: no hook compare, no dispatch entry, no census line. Today a
   disabled fix still walks the ladder on every eye draw, so removing that
   is Phase 1's first win.
2. Draw-path interest is decided when a shader is bound, not per draw. The
   binding shadows already see every shader set; the core looks the shader
   up once in its interest table, caches the answer, and a draw only reads
   that cached answer and calls the plugins it names.
3. The plugin layer adds no D3D calls. Plugins read state from the binding
   shadows, never with Get* calls on a hot path (the query cut, everywhere),
   and the census counts D3D calls per plugin.
4. No virtual fan-out, `std::function`, heap allocation or lock on the
   render thread's draw path (engine motion took a recursive mutex twice a
   draw until 2026-09-30). Dispatch tables are flat arrays, rebuilt only on
   configure.
5. The same rules on the runtime side: no plugin callback in the XR frame's
   critical path unless the plugin subscribed to it.
6. Budgets and gates, relative rather than absolute (Sean, 2026-09-30):
   - each plugin's manifest states a per-frame budget (render-thread CPU,
     D3D calls, GPU time), and the census reports every plugin against it;
   - a build rig replays a recorded draw stream through the dispatch and
     fails when the cost per draw exceeds the previous build's;
   - every migration phase is flown at the same spots (the Epic hangar in
     flat; the carrier and on foot in VR; anti-aliasing off and on) and
     ships only if its census shows EDVR's CPU and GPU cost no worse than
     the previous phase's, within noise.

## 8. Migration

- **Phase 0:** this document and the decisions in section 10.
- **Phase 1:** the registry, manifests and dispatch tables in the core,
  inside the one DLL; the ladder as data; per-plugin configure and census
  families. Move night vision first (small, and it already has a rig). The
  gate is byte-identical verdicts on replayed censuses plus the existing
  rigs, and a census at the same spots showing EDVR's render-thread cost
  lower than today's (disabled features stop walking the ladder).
  The registry also owns the draw-gate subscriptions (wake pulse and night
  vision were missing from `drawGateSubscribed` until d58e4cd0, 2026-09-30;
  a subscriber the list forgets starves silently), and static props and the
  scheduler stack probe stop hanging off `temporalPassConfigure`
  (temporal_pass.cpp 6149-6154), or deselecting temporal-aa orphans them.
- **Phase 2:** move the other groups one at a time, each with its rigs and
  one flight: cockpit-visuals, exposure, scanners, intro, on-foot-panel,
  comfort, performance, temporal-aa (with UI quality) last. Each move adds a
  verdict replay fixture for the features that have no rig today.
- **Phase 3:** installer component selection (profile-filtered, dependencies
  auto-selected, a recommended set), receipts that list plugins and
  versions, Modify without touching settings, F8 rows only for installed
  plugins, the contract gate per plugin, and a startup line naming the plugin
  set (`edvr_log.py --expect-plugins`).
- **Phase 4:** the add-on tier of section 6. (Separate first-party DLLs are
  not planned: Q1 chose monolithic.)
- **Phase 5 (Q7):** graphics-only VR, after Phase 1 and Phase 3's component
  selection: the four changes of Q7, then a flight on a stock runtime before
  it ships.

## 9. Risks

- **Behaviour drift while moving code.** Mitigated by the verdict replays,
  the trace replays and one flight per group.
- **Coupled features split across plugins.** Kept together (temporal-aa);
  services where there is a real consumer.
- **Combinations.** Per-plugin rigs plus a small supported matrix (temporal
  on and off, UI quality off, 100 and 125, VR and flat) rather than every
  subset.
- **The installer's modify and repair paths** grow; receipts already
  fingerprint every file.
- **Runtime-side features** (cull guard, FOV, resolution, pacing, flash) need
  hook points in the OpenXR runtime as well; the contract spans both DLLs.

## 10. Decisions for Sean

- **Q1 (decided 2026-09-30): monolithic.** Separate plugin DLLs would add a
  C-only boundary (/MT gives each DLL its own CRT heap, so nothing may be
  allocated on one side and freed on the other), calls on per-draw paths
  that cannot be inlined, a signed binary per plugin and more for antivirus
  heuristics to flag (EDVR has a quarantine history), installer file sets
  that vary with the selection, and hash-checked loading. Version-locking
  means they would not buy independent updates either. Monolithic keeps one
  binary, inlinable dispatch, simple installs and plugin toggles without a
  reinstall; the build enforces the boundaries. Revisit only if independent
  plugin updates or third-party draw-level plugins become real needs.
- **Q2 (decided 2026-09-30): the nine plugins of section 4.** temporal-aa
  stays one plugin: its parts share per-draw state, the camera and the
  history, and splitting them would put that traffic on a plugin boundary,
  against the north star. Sean regrouped the same day: the intro fixes
  (intro video, splash backdrop, loading dim, and the loading hologram) and
  the on-foot panel (black void, screen resolution, panel distance,
  curvature, weapon stability) become plugins of their own, replacing
  "screens", and UI quality ships only with temporal AA, inside that plugin.
- **Q3 (decided): defaults reproduce today's shipped behaviour exactly.**
  The recommended VR set is every plugin but diagnostics, with temporal-aa
  and performance installed and their features at today's defaults; flat
  is temporal-aa only.
- **Q4 (decided): the add-on tier comes after Phase 1,** built on the
  registry and manifests. Meanwhile PR #46's OM-unbind fix lands on its own,
  and Devin gets section 2's defect list so his rework lines up.
- **Q5 (decided): diagnostics probes, dumps and developer instruments form
  the diagnostics plugin, not installed by default.** The per-frame census,
  the LONG FRAME lines and the version and plugin lines stay in the core:
  field reports are diagnosed from them.
- **Q6 (decided): today's keys and sections stay,** each owned by one plugin
  in the config contract (check_config_contract.py gains the owner). The
  unread-key audit says "belongs to <plugin>, not installed" instead of
  "not read"; a fresh install writes only the installed plugins' blocks;
  existing ini files keep working untouched.
- **Q7 (decided 2026-09-30): graphics-only VR is its own phase, after Phase
  1 (section 8, Phase 5).** When no selected plugin needs the runtime
  (section 4.1), the installer skips the runtime set and Elite stays on its
  stock VR path (SteamVR's OpenVR, or LibOVR), with EDVR's d3d11 fixes on
  top. Four changes first: (a) a graphics image that does not assume the
  runtime: a second build (Oculus route inert, startup marker flags 0, the
  shape 1a54e9e removed) or a load-time check; DllMain cannot read the ini
  (oculus_route.h:22-25), so the decision is build-time or leaves DllMain;
  (b) a hardened eye-size fallback: the "2048 on both axes" guess silently
  disables every eye-branch fix on smaller headsets, so vScreenIsEyeSized and
  splash_dim adopt the targetIsEyeSized rule; (c) dependency enforcement at
  plugin registration, from the install record (the module list settles up
  to a second late); (d) edition ini defaults generated the way
  edvr-flat.ini is (`intro_video=head`, `fss_eye_sync=sync`,
  `vscreen_res_width=2880`). The installer says plainly what the user gives
  up: no F8 menu in VR (settings through the installer's settings window or
  the ini; live reload works), no DLSS, FSR, TAA or UI quality, no flash
  fix, no Explorer Cam. Not planned: any of those without the runtime; that
  is rebuilding the OpenVR proxy deleted 2026-09-16 (1a54e9e). The first
  build needs a flight on a stock runtime: the graphics half has not run on
  one since then. Risks: LibOVR users move off LibOVR today (the route is
  unconditional, d3d11_proxy.cpp:660); the native image refuses unknown game
  revisions (DllMain returns FALSE); `temporal_aa` or FSS `on` without the
  runtime costs with no output; the vr_runtime "Reinstall" advice
  (vr_runtime.cpp:106-113) and the README's second-log guidance turn wrong.

## 11. Implementation review, 2026-10-01

Sean requested review and implementation on a separate branch, with Luna 6
agents working in parallel, and authorized the Steam install for testing.
The branch is `codex/plugin-architecture`, based on `14a7ff70`. Main stays
unchanged until explicit shipping authorization.

The review found these constraints before implementation:

- Cache the candidate claims at shader bind/configuration changes. Final
  verdicts still depend on draw shape, resources, failure state and earlier
  claims. Preserve the night-vision claim's existing ladder position.
- The current census lacks enough state to replay every classifier. The
  pilot may compare a frozen legacy night-vision classifier with the new
  dispatch, including earlier-claim outcomes, but that proves only the
  pilot. Whole-ladder migration needs recorded resource/state fixtures and
  comparison of composed actions as well as verdict IDs.
- Canonical, versioned manifest data must generate the compiled catalog.
  Validate unique IDs, dependencies, profiles and explicit ownership of
  each current config key. Shared section names cannot determine ownership.
- Keep the installer's existing payload `components` field intact. Plugin
  selections need their own versioned record in the later installer phase.
  Soft dependencies do not auto-select plugins.
- Registry subscription publication must cover configure and reload;
  dynamic probe arming still needs its current refresh path. Static props
  and the scheduler probe must be independently configured without losing
  refresh behavior on engine changes.
- Keep the default feature behavior throughout this first slice. Catalog
  defaults describe the final architecture; they do not imply that legacy
  diagnostics or any unmigrated feature is already suppressed.

Implementation boundaries: one agent owns the manifest, generated catalog
and config ownership checks; one owns the registry, night-vision pilot and
scoped replay; one owns the include-boundary gate, static-library build and
rig wiring. The coordinator owns integration review, the full build, Git
and Steam installation. No group migration or add-on loading bypasses the
flight gates in section 8.

Next flight: compare unchanged `14a7ff70` and the reviewed pilot at the same
carrier and on-foot spots, AA off and on, with identical settings, headset,
per-eye size, VR runtime and DLSS version recorded. Flat comparison uses the
same hangar spot. Verify log build identity first. Require repeatable CPU
improvement beyond observed noise for Phase 1; unchanged or inconclusive
measurements do not satisfy the Phase 1 improvement gate. GPU cost must not
regress. No performance improvement is claimed before these flights.

Baseline preparation: the unchanged full build passed all gates and wrote
its receipt. The build-runner process-tree termination self-test failed
inside the sandbox and passed unchanged outside it, so full validation ran
outside the sandbox. The sanctioned installer installed and verified Steam
with backups and without overwriting `edvr.ini`. Sean reports Pimax with
Pimax OpenXR. The requested comparison includes game night vision on and off;
the completed refly omitted that toggle, so no baseline claim draw was tested.

Integration review caught and repaired a missing core dispatch include and
a rig counter that confused shape evaluation with resolver invocation. It
also moved new bind/draw activity breadcrumbs to frame-boundary reporting:
the dispatch rig now checks that neither hot path calls the logger and that
reports drain once, including at shutdown (424 checks passed). An unchanged
heartbeat stress assertion failed under the first full runner load; H5 passed
five focused runs and the next full run with four jobs. No heartbeat code or
test was changed. That full run then stopped at the footprint rig's old
source-layout assertion for draw-gate membership. Its subscription contract
must follow the new registry before the full build can pass. The baseline
flight began during compilation; only samples after compilation stops can
support the performance comparison. No candidate has been installed.

Cache-coherence audit found shader-shadow repairs outside the binding hooks
(engine velocity and scanner resolve), plus exposure's independent reset.
An old per-draw hash read saw those changes; hook-only cached candidates can
miss them. Publish from the canonical shadow write/reset points, with an
observer only for active plugin interest and a current-shadow seed when
configuration enables interest. Preserve profile bypass and add repair,
reset and off-bind-on checks before the next full build. Sean reduced OpenXR
resolution to 4032 per eye and will refly; the earlier 4508x4358 samples are
not the comparison baseline. Verify dimensions and build in the new log.

Next replay slice, from the source inventory: the 19 verdicts include `None`;
`Skip` and `Backdrop` each have multiple distinct claim sites. Preserve the
early/offscreen route as well as the eye ladder. A normalized capture needs
profile/config and frame latches, derived resource/CB facts, ordered rung
outcomes (`not visited`, decline, pass), claim identity and final verdict.
Record composed actions after forwarding too: begin/end, original issue,
swallow, curve/dim and UI-layer reissues. The existing DC census precedes
later claims and has no final verdict or complete classifier state. Keep it
intact; add an explicitly armed sidecar using already-read facts, without
extra D3D queries or a draw-path logger while unarmed. Recorded predicate
outcomes alone prove ordering, not classifier equivalence; synthetic
conflicting-claim cases and module-specific resource/state rigs remain
necessary. Whole-ladder replay and performance gates are still outstanding.

Baseline refly evidence: `edvr_gfx_20261001_171838.log`, v0.18.0 /
`6ABED2A4`, unchanged `14a7ff70` payload verified. Pimax Crystal Super with
Pimax OpenXR at 90 Hz; output 4032x3898, submitted ROI 2016x1949. Preset K
is logged; the loaded DLSS DLL version is not read by this build. Sean
confirms carrier first, then an on-foot hangar, AA off then DLSS in each.

Direct EDVR draw-hook CPU estimates (window ends; approximately 20 seconds
at 90 Hz; 1/16 frames, every 64th draw scaled by 64):

| Scene / AA | Window end | Mean / max ms |
|---|---|---|
| Carrier, off (last pure pre-toggle window) | 17:20:58.196 | 0.411 / 1.984 |
| Carrier, DLSS | 17:21:38.357 | 1.310 / 4.608 |
| Carrier, DLSS | 17:21:58.446 | 1.465 / 4.282 |
| Carrier, DLSS | 17:22:18.671 | 1.207 / 3.085 |
| On foot, off (after Disembark) | 17:23:19.864 | 0.226 / 0.627 |
| On foot, DLSS | 17:23:59.907 | 0.422 / 5.485 |

These exclude forwarded game draw time and include EDVR reissues; they are
means/maxima, not percentiles or all EDVR CPU. Early off windows vary from
0.007 to 0.411 ms, so scene/workload matching matters. Exclude windows that
straddle AA or disembark transitions. Native benchmark CPU measures the
broader application render path (game plus EDVR); GPU samples likewise
include game work. On-foot off window 14 completed 30 s: CPU/GPU p50
1.861/5.173 ms. On-foot DLSS window 16 closed at exit after 29.641 s with
`scope-changed`: CPU/GPU p50 2.335/8.183 ms, not a completed window. Keep
this status when comparing; no improvement or no-regression claim yet.

The remaining full-run failure was the transport-only fixture leaving
night-vision stability at its true default. Module-owned startup demand
correctly keeps optional hooks installed in that case. Make the fixture
explicitly switch both night-vision settings off, and retain its real WARP
transport matrix; do not weaken the hook assertions or production demand.
The candidate's four timed cases must keep game night vision off to match
this baseline; exercise ship-cockpit night vision afterward as a separate
pilot check. Full-ladder replay, the rest of the phases and shipping remain
outstanding.

### Candidate flight, 2026-10-01

Full validation passed all gates (120 runner jobs), including 427 dispatch
checks, production DLLs and both self-contained installer profiles. The
validated source was committed and pushed as `3f8dceec`; clean-version
DLL-only promotion passed. Steam installation and verification preserved
the live INI. Main is unmerged.

Candidate log `edvr_gfx_20261001_192343.log` matches
`v0.18.0-1-g3f8dceec` / `6ABEF27A`, linked 2026-10-01 23:53:30 UTC.
Runtime, headset, 90 Hz, 4032x3898 output, 2016x1949 submitted input and
DLSS preset K match baseline. The loaded DLSS DLL version is still unlogged.
Sean flew on-foot hangar DLSS then off, carrier off then DLSS, then NV.

| Scene / AA | Candidate window end | Mean / max ms | Interpretation |
|---|---|---|---|
| On foot, DLSS | 19:25:52.354 | 0.439 / 5.568 | Baseline 0.422 / 5.485; near equal, not improvement |
| Off, uncertain scene boundary | 19:26:33.008 | 0.196 / 0.397 | May straddle hangar-to-carrier; exclude scene-specific comparison |
| Likely carrier, off | 19:26:53.030 | 0.248 / 0.742 | Scene assignment inferred from Sean's sequence; one bucket |
| Carrier, DLSS | 19:27:33.058 | 1.012 / 3.450 | Mean below baseline 1.207-1.465; one bucket |

The table uses the same sampled direct draw-hook metric as baseline, with
112 or 113 sampled frames per bucket. Exclude the off bucket ending
19:26:12.355 and DLSS bucket ending 19:27:13.057 because they straddle AA
toggles (19:25:56.490 and 19:27:03.036-19:27:03.500). The bucket ending
19:27:53.057 straddles NV engagement at 19:27:37.567. There is no later pure
NV draw-hook report. The log does not identify the exact hangar-to-carrier
boundary. No clean scene-specific hangar-off bucket or repeatable CPU
improvement is established; the Phase 1 performance gate stays open.

Broader application benchmark p50 CPU/GPU: on-foot DLSS window 9 completed
30 s, 2.184/8.603 ms; off window 11 covered 30 s but was scope-changed,
1.814/5.655; carrier DLSS window 13 completed 30 s before NV,
5.090/10.609. NV window 14 was scope-changed after 29.907 s,
4.606/10.346. These include game work and cannot prove isolated EDVR CPU
improvement or GPU non-regression.

NV correctness evidence: the VR profile registered the cockpit plugin,
observed the shader pair and evaluated its live claim. Pixel-shader
creation succeeded; stock-appearance, pulse-stability-on NV engaged at
2016x1949. The later GPU census records 0.47 wrapped NV draws/frame and
0.025 ms. No NV failure or absorbed fault is logged. Against baseline,
the shader header and begin/end/exterior-mask bodies are unchanged; the
hashes, X/240/1 shape and rung before RemLok are preserved. This proves
execution and static parity, not visual equivalence or shutdown.

Sean reports slight NV blur, possibly pre-existing, and no other visual
issues. No baseline NV sample exists. Hypotheses: existing NV/DLSS behavior
(same blur on baseline), or migration regression (candidate-only blur in
the same scene/settings). Current diagnostics do not discriminate them.
The sanctioned installer restored and verified baseline `14a7ff70`, keeping
the live INI, for one 30-second cockpit DLSS+NV comparison. Keep the shader
and rendering behavior unchanged until this comparison supplies evidence.

Focused NV comparison completed: `edvr_gfx_20261001_193846.log` matches
baseline `14a7ff70`, v0.18.0 / `6ABED2A4`. DLSS engaged at 4032x3898;
stock-appearance, pulse-stability-on NV engaged at 2016x1949 at
19:40:23.779. Sean reports the same blur on baseline and candidate.
Ruled out: plugin migration introduced NV blur, because the verified
unchanged baseline reproduces it in the matched comparison. No NV rendering
change is included. Elite has exited; scoped checks can resume.

Next Phase 1 slice: compile-time ordered claim/observation descriptors must
drive production evaluation, avoiding per-draw function-pointer fan-out.
Parallel owners are the shared ladder/rig/build wiring, bounded armed trace
storage/codec/reader, and single-owner vScreen integration. Preserve all
routes and composed forwarding actions. Flat retains its existing VR-ladder
bypass; Auto and indirect commands get distinct bypass records, with GPU
argument contents explicitly unavailable. Production capture covers owner
immediate-context hook commands; foreign-context exit ordering has synthetic
coverage. An explicit core `advanced.draw_replay` switch defaults
off; a manual census request may arm one next complete frame, while
automatic censuses never do. This ordering/action replay does not prove
resource/constant-buffer classifier equivalence by itself; module rigs and
the remaining performance gate still apply.

The guarded local ladder rig passed a 41-draw terminal matrix (682 site
events, 91 actions), including every terminal claim/exit, flat Auto and
indirect commands, zero-instance draws and internal-world bypasses. It
rejects invalid tokens, post-finalize writes, duplicate forwarding facts,
config reloads during capture, site/action overflow and unfinished draws;
reader dry-run snapshots are unchanged. These are synthetic selector/codec
checks, not evidence of resource-classifier equivalence or improved flight
performance. Production compilation and independent integration review
passed; the full build remains required.

Optimized production inspection caught retained typed-rung calls; explicit
inlining removes them. The current unarmed classifier has zero calls to
`visit*`/`visitOrdered` and no stack-cookie check, as on baseline. Its stack
frame is 288 bytes versus 272 on unchanged `14a7ff70`, and emitted code is
approximately 192 bytes larger. This is compile evidence, not a flight
performance result. A separate helper audit found trace-only issue counters
in the unarmed path; they are removed, with exact single-issue trace events
using literal counts. Optimized helper inspection shows no remaining trace
counter increments. The mechanical ladder/codec snapshot gets full
validation before the disabled-interest and census work; it does not need
a separate headset flight.

Lifecycle review found replay storage configured before failed or dormant
hook installs, with no shutdown release. Configuration now follows a
successful hook commit. Cold shutdown follows hook removal, discards any
armed or partial capture with an explicit breadcrumb, invalidates tokens and
releases storage without writing a completed-frame claim. The guarded rig
passes partial-capture, armed-only and repeated-shutdown checks.

Focused contract validation passes: UI quality 3,215 checks and its WARP
seed test 385,610 checks; composite census 51 checks and 50 mutations;
on-foot maps 80 checks and 53 mutations; world-route mutation self-test
96 cases. The standalone replay fixture initially lacked a build version
because its wrapper did not inherit the full build's version environment;
the strict reader rejected it. Correcting that test environment makes the
writer/reader roundtrip pass without relaxing identity validation. Full
validation runs with four jobs; Steam remains on baseline. Its first attempt
stopped on four flat-substitution source pins. Ruled out: missing production
clear/copy events, because each hook contains its one expected event; the
binary-text parser crossed CRLF-closed handlers into later LF-closed hooks.
Normalizing line endings fixes the scoped checks, and an adjacent-hook
fixture catches the failure. The guarded flat collector rig passes.

The retry passes the complete validation build: 115 pooled jobs plus six
quiet jobs, both production profiles, self-contained installers, 232-key
configuration contract, and package checks. Log:
`build/plugin-architecture-ladder-build-2.log`; full-build receipt input hash
`2959b46eb8fef6b69e2a8d9ea138fdc3ce93b52211275026556e5108f59064d8`.
This validates the mechanical ladder/replay slice. Disabled-feature
interest, per-plugin cost attribution and the Phase 1 performance flight
remain outstanding; Steam is unchanged on baseline `14a7ff70`.

Next interest slice: module-owned cold metadata publishes conservative
configuration interest for target sharpening, witchspace-star hiding, FSS
panel/reveal/dump, particle substitution and the two curvature observers.
The canonical binding observer caches known shader matches; unknown hashes
retain the existing fallback. The typed ladder reads that legacy mask lazily
and skips disabled predicates, while preserving all site IDs and trace
`NotEligible` events. Night vision retains its separate mask at its current
rung. Core/shared observers and dynamic readiness, faults, retirement,
resources and body/frame state stay live. Flat never subscribes to this
VR-classifier cache. The current settings enable curvature and several
cockpit features, so config-off checks alone cannot establish a workload
win; codegen checks and a matched performance flight remain required.

The guarded interest checks pass: registry dispatch has 458 checks, and the
production replay roundtrip covers 44 draws, 685 site records and 102 action
records. Disabled optional claims and observers record `NotEligible` while
mandatory/shared observers retain their order. Canonical bind repair,
reset, configuration reseeding and flat bypass remain covered. The first
optimized visitor retained eight loaded-flag guards; using the known first
legacy-interest rung removes them. Independent optimized inspection finds
one legacy mask load, 94 calls, a 288-byte stack frame and no selector,
trace, or stack-cookie calls in the unarmed classifier. Its last instruction
starts at offset 7,052 versus 6,885 for validated `d57600de` and 6,693 for baseline
`14a7ff70`; baseline has 105 calls and a 272-byte stack frame. These are
compiler checks, not a measured flight improvement. This changed source
still needs full validation before commit or installation.

Cost review: all nine manifest budgets remain explicitly unmeasured, with
CPU, issued-D3D-call and direct-GPU metrics and fixed coverage descriptions.
Future relative limits require a build/profile/scene/config reference and
measured uncertainty; the permitted increase cannot exceed the reference
interval's positive noise margin. Existing GPU samples are being grouped
by logical owner without changing timestamp collection. Nested temporal
breakdowns and wrapped game draws are excluded from owner direct-work
subtotals. Quiet scopes do not imply missing samples, and a shared
weapon-motion service is charged once to temporal-aa. These partial scope
estimates do not establish full plugin costs or flight budgets.

The guarded GPU rig passes 326 checks with warning-free compilation and a
clean focus guard. Its semantic ownership table pins every existing census
section, excludes nested/wrapped work from direct subtotals, keeps core
separate, distinguishes quiet, omitted and untimed scopes, and emits no
inactive-owner rows. The shared weapon-motion note requires that particular
scope to have occurred. New owner rows follow the original detailed group,
preserving the main-line/seed-detail adjacency. No timestamps, shaders or
D3D behavior changed. Catalog self-tests and emitter dry-run pass; current
source is frozen for full validation.

The first full validation attempt stops at UI-quality source-order pins
after 22 of 121 jobs. Ruled out: curvature recognition moved before the
movie claim or after the backdrop, because the production sequence retains
both positions and the same six-index-X/`introCurveWants()` predicate. The
pins and mutation anchors still spell those rungs as ungated `Site` types;
they must follow the new `InterestGated` declarations without relaxing the
ordering contract. Related mutation fixtures are being audited together.

The repaired pins pass 3,215 UI-quality checks and 385,610 WARP seed checks;
the guarded world-route rig passes 283 checks and all 96 mutations. Both
focus guards are clean. Full retry `build/plugin-architecture-interest-build-2.log`
passes all 121 jobs (115 pooled, six quiet), both production profiles,
self-contained installers and the 232-key configuration contract. Receipt
input fingerprint:
`86bddc5f26093cefe7938498f65d3c1b184e3c161bd108bf897cd98bce4ac253`.
This validates disabled-interest, structured unmeasured cost metadata and
existing GPU-scope attribution. It does not pass the Phase 1 flight gate or
complete CPU/D3D-call attribution. Steam remains on baseline `14a7ff70`.

The CPU/call inventory also found a limitation in the existing aggregate
draw-hook estimate: indexed-instanced forwarding times its weapon-motion
reissue inside the interval subtracted as the real call. Consequently that
particular EDVR reissue is excluded despite the broader log description.
Keep the existing aggregate comparable between builds; new per-owner
measurements need explicit timing boundaries and coverage. GPU-census scope
occurrences cannot substitute for executed D3D-call counts. Per-owner CPU
and actual issuance annotations are still required before the next Phase 1
performance flight.

### Sampled cost collector, 2026-10-02 (validated)

The next slice times typed classifier handlers only after their cached
interest gate admits them. A gated miss records Reached and NotEligible
without reading the clock; an eligible handler records Invoked and a timed
scope. Foreign/deferred contexts, flat/internal/indirect bypasses and replay
captures do not use the sampled CPU policy. NoCpu removes its scopes and
counter callbacks at compile time.

CPU probes use ordinal 32 within the existing 64-draw sampled-frame stride;
the comparable aggregate sample remains at ordinal 0. Sharing the aggregate
draw would put extra probe clocks inside that sample and multiply their cost
by 64. The existing aggregate's forwarding boundaries stay unchanged,
including the indexed-instanced weapon-motion limitation recorded above.
CPU estimates include probe overhead and cover classifier handlers, not all
module work. They use actual completed, unsuppressed sampled frames, include
owner-zero samples, and report frame-level spread with no confidence interval
or calibrated budget claim.

API notes count actual direct context invocations in the migrated NV module,
including getters, state changes and restoration, with separate Work,
Transfer, State, ReadQuery and Instrumentation classes. Loop iterations count
separately. The immediate-context check remains one existing query; rejected
deferred calls do not touch collection. An accepted NV begin reads sampling
once and retains it through the matching end, so unsampled API sites do not
call the collector. Helper internals, device/resource creation, IUnknown,
SDK/runtime work, game forwards and other modules remain outside this slice.
API counts are raw totals and per completed API sample frame, never CPU-stride
scaled or inferred from GPU scope occurrences.

The C-compatible fixed-memory collector drains closed sample flags before
the frame monitor resets and publishes the next flags. Configure/reload drops
mixed partial state and one subsequent close; shutdown discards partial data
after unhooking. A replay latch excludes its CPU frame while retaining API
sampling. Sparse rows name logical owners and observed source coverage; they
do not imply module selection. Cold configure/window lines distinguish a
running collector with no observations from a collector that never ran.
The first optimized production compile passes without warnings and the NoCpu
body has no new collector or clock callbacks. Its listing uses `/FAs`, so its
machine-code extent cannot yet be compared to the saved `/FAcs` reference.
Ruled out: relocation offsets of 587/618 represented classifier byte sizes,
because they were PDATA references, not encoded instruction offsets. The
saved `b1c6edf6` reference ends at instruction offset 0x1b8c followed by a
five-byte jump: 7,057 bytes including that instruction. Earlier 7,052 was
the last instruction's start offset.

The final type-only CPU policy keeps the old trace/context/draw-argument ABI;
an unused policy-reference parameter was removed before validation. A later
independent re-audit of saved and fresh `/FAcs` listings confirms matching
7,057-byte bodies, 1,636 instruction-offset rows, 94 calls and a 288-byte
stack reservation. The earlier instruction count and checksum were not
supported by those artifacts. Concatenated encoded bytes match SHA-256
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`.
The NoCpu classifier has no collector, clock, selector-helper or stack-cookie
calls. Direct draw thunks grow by 40-50 bytes while retaining their call counts
and stack reservations. These are codegen checks, not a flight result.

The serial guarded `run_jobs.py` set passes `plugin_cost_test`,
`draw_ladder_test` and `native_motion_rigs`, without compiler warnings or
foreground/window events. A real `/TC /std:c11` smoke pins the C record sizes,
offsets and function signatures. Collector fixtures cover actual closed-frame
denominators, stride scaling only for CPU, replay suppression, measured zero
versus absent rows, configure/reload and shutdown. Fake-clock selector fixtures
cover gated misses without clocks, exact eligible timing, NoCpu with trace,
and a valid earlier claimant preventing later NV queries or scopes. NV WARP
passes 264,393 checks, including no unsampled notes, exact pulse-path 11 calls
(nine read/query, two state), the repeated SRV loop, three-query malformed
settings return, state restoration and deferred rejection before collection.
The first full validation attempt stopped after 56 of 122 jobs at the VR
world-route mutation fixture. Its unchanged production checks pass 283 cases,
but the `iw-flag-put-away-before-the-draw` mutant could not find its old
`beginPanelOverride(trace, ...)` source anchor after the type-only CPU policy
changed that call spelling. The mutation was not applied or run; this failure
does not measure a rendering or cost regression. Preserve its original
semantics and update scoped anchors before rerunning full validation.
The anchor repair passes the guarded 283 route checks and all 96 mutation
checks over four rigs. The unchanged production code and repaired fixtures
then pass the full normal `build.bat --jobs 4`: 116 pooled jobs in 138.8 s
and six quiet jobs in 25.5 s, both production profiles, self-contained
installers and 232-key config/package contracts. Receipt created
2026-10-02T06:40:42.483870Z, status `full-pass`, inputs fingerprint
`bca56d8554df2336a3459036fb389812ffa63d7d42c5dc9028e951836d6c0372`.
This validates the partial collector; it does not complete all-module CPU/API
coverage or pass the Phase 1 performance flight gate. Steam remains on
`14a7ff70`; repeatable baseline/candidate windows are still required.

### Performance comparison baseline, 2026-10-02

The longer Steam baseline is `edvr_gfx_20261002_044730.log`, verified by
`edvr_log.py --expect-build 14a7ff70 --version`; its paired native log is
`edvr_openxr_20261002_044731_538_29788.log`. Pimax Crystal Super / Pimax
OpenXR remains at 90 Hz, 4032x3898 output and 2016x1949 DLSS input per eye.
The loaded DLSS library version is not logged. Night vision was off.

The log records off at 04:48:06, DLSS at 04:51:29, the on-foot hangar marker
at 04:53:41, and off again at 04:54:56. Thus the hangar starts within the
existing DLSS span. Carrier attribution comes from Sean's flight sequence;
there is no explicit carrier marker. Exclude startup, AA transition windows
and the window crossing the hangar marker. Possible movement before that
marker remains a comparison limitation. The clean carrier-off aggregate
windows span 0.335-0.351 ms/frame; candidate data is still pending.
These are sampled, scaled hook means, not percentiles or complete EDVR CPU.
The existing weapon-reissue subtraction limitation remains unchanged.

Clean-version promotion of the validated source passed as
`v0.18.0-5-gee2628da`. Steam candidate installation and independent
`install_edvr.py --all --ini --verify-only` passed. Staged DLLs match the
promoted outputs; personal INI bytes differ only by `draw_replay = on`
under `[advanced]`. The requested candidate sequence matches the logged
baseline order, with 90 seconds per scene/mode, followed by a separate
60-second NV sample and one manual replay capture. Builds and GPU rigs stay
idle during the comparison. Phase 1 performance and full replay gates remain
open; no further group migration is accepted yet.

### Candidate performance and replay flight, 2026-10-02

`edvr_gfx_20261002_054152.log` passes the pinned `ee2628da` build check
(`v0.18.0-5-gee2628da`, PE stamp `6ABF5388`). The paired runtime log is
`edvr_openxr_20261002_054154_441_20312.log`, also the candidate. Runtime,
headset, refresh, 2016x1949 input, 4032x3898 output, FOV and preset K match
baseline. The loaded DLSS/driver version remains unlogged. Sean flew hangar
off then DLSS, carrier DLSS then off, and finally cockpit NV with DLSS.
He reports the visuals looked the same. Exclude startup, mode/scene
transitions, the final NV period and the manual capture from comparisons.

Clean sampled hook means are descriptive, not whole-CPU or percentile data:

| User-labelled context | AA | Baseline ms/frame | Candidate ms/frame |
|---|---|---:|---:|
| On-foot hangar | off | 0.235 | 0.178 |
| On-foot hangar | DLSS | 0.428 | 0.360 |
| Aboard carrier | DLSS | 1.028 | 0.814 |
| Aboard carrier | off | 0.343 | 0.374 |

The carrier-off baseline here uses its four steadier windows (14400-19800),
excluding the early 0.285 settling window. Candidate carrier-off windows
are 0.332, 0.378, 0.382 and 0.404: the first matches baseline, then rises.
Candidate carrier DLSS has only two clean windows. No per-window mean draw
count exists, and the exact carrier/camera is not instrumented. This does
not prove a code regression or permit dismissing the difference as noise.
The native CPU/GPU percentiles include game/runtime work and use unequal
completed-window counts; they cannot supply missing EDVR attribution.

Direct EDVR GPU subtotal medians favor the candidate in the other three
cells. Carrier off is 0.018-0.019 versus 0.012 ms/frame; this difference is
entirely Core's `DoorMenu`, with two occurrences/frame in both flights.
Uncertainty is unmeasured. Wrapped NV GPU time includes the whole game draw,
and DLSS observes only seven of fourteen direct temporal-AA scopes. These
partial observations do not pass GPU non-regression or calibrate budgets.

NumLock, now mapped to `dump_draws`, produced armed/capturing/complete-written
at 05:50:57.685-766. The strict sidecar reader validates frame 48665: 3,892
draws, 129,458 site events, 12,100 action events; 762 offscreen and 3,130 eye
draws, with two NV wins. This is selector/action-order evidence, not full
predicate parity. The NV sample emits 452 State and 2,034 Read/Query calls
across 113 sampled frames and ten actual annotated sites, proving the new
API path ran. Those rows cover annotations only and have no baseline analogue.

Open discriminators: more/different hook work needs a timed-draw denominator;
added per-draw overhead needs same-workload normalized timing; the tiny
`DoorMenu` GPU difference needs repeatable scope samples. Instrument these
before requesting another comparison. Three Luna slices now extend the
denominator, actual Target/RemLok API notes and independent source-fact
replay for draw-gate and inclusive eye-range predicates. Full validation
and production fact-capture evidence remain required; Phase 1 stays open.

### Phase 1 evidence extensions, 2026-10-02 (validated)

Three Luna implementation slices were reviewed together before the normal
full build. All 123 jobs pass: 117 pool jobs in 153.4 seconds and six quiet
jobs in 26.0 seconds, both production profiles and self-contained installers,
and the 232-key config contract. The full-pass receipt at
`2026-10-02T13:29:45.418299+00:00` fingerprints source inputs
`8a321110800d436c98afe82328870bdd6bf607f17bda8cfeab415f195af2832e`.
The build log is `build/plugin-architecture-phase1-evidence-build-3.log`.

- Hook CPU now reports a weighted mean per actually timed draw and its
  sample count, alongside the existing scaled per-frame mean/max. It reuses
  existing clocks and preserves the frame-level clamp, sample stride and
  forwarding boundaries; zero valid timed samples remain unavailable.
- TargetSharp and RemLok annotate their actual context API calls with stable
  source sites. Cold hook registration and a frame-boundary thread token
  restrict sampling to the registered immediate context and render thread.
  Review extended that guard to NV while preserving its existing `GetType`
  rejection and rendering calls. The real Target/RemLok WARP rig passes
  56 checks, including restores, declines and replacement failures. The NV
  rig passes 264,397 checks, including a sequential foreign-thread call with
  the same context pointer that restores state and emits no cost notes.
- Trace schema 2 records the existing draw-gate scalar and inclusive eye-range
  inputs, plus the observed skipped-counter delta. Separately coded frozen
  `14a7ff70` predicates check only these two families. Unknown inputs differ
  from Off and known-empty ranges. Writer checks reject missing, duplicate
  or invalid visited facts. The C++ writer's terminal matrix produces 49
  matching selector events, zero mismatches or unreplayable facts, and no
  missing mutation observation; both strict readers accept it. CLI/build
  gates now fail on supported predicate mismatch, unknown input or missing
  mutation observation. Schema 1 remains valid historical selector/order
  evidence with predicate status unavailable. Whole-ladder equivalence
  remains false, and a production v2 capture is still required.

Follow-up review caught a trace-only 32-bit snapshot of the 64-bit census
counter. It now preserves the source width and bounds the 64-bit delta before
narrowing; changes larger than one invalidate the capture. Tests cover high
counter values, unsigned wraparound and invalid-delta rejection. The normal
full build passes after this C++ correction. Its first repeat stopped at the
final rig's stale-data guard: the previous passing run left new negative
fixtures outside the runner's cleanup namespace. The fixture root now uses
`draw_ladder_test-trace`; explicit cleanup includes the three new negative
cases. Two consecutive guarded focused runs and the final full run pass and
leave no stale root. The startup guard and dry-run file comparison remain.

Two independent listing parsers confirm that the fresh `NoTrace, NoCpu`
classifier remains byte-identical to its saved reference: 7,057 encoded bytes,
94 calls and a 288-byte stack reservation, SHA-256 `2647f030...`. Neither trace
fact tap adds work to that body. This proof covers the unarmed classifier
specialization, not the whole draw path or historical `14a7ff70` performance.

Cost counts cover annotated methods and classifier samples, not whole-plugin
CPU/API/GPU costs; all nine manifest budgets remain unmeasured. An outcome-only
schema-1 stream can measure traversal/gating policy overhead, but cannot
reconstruct hidden predicates or serve as a historical whole-classifier
performance baseline. The independent relative cost rig, remaining predicate
facts, carrier-off comparison and GPU non-regression gate remain open. The
validated extensions do not authorize Phase 2 migration or shipping.

Candidate `03049f8d` is committed and pushed on `codex/plugin-architecture`;
it is not an ancestor of main. Its clean receipt-guarded promotion passes and
produces `v0.18.0-7-g03049f8d` in both Steam DLL product versions. The sanctioned
installer dry-run writes nothing; install and separate `--all --verify-only`
pass. No `--ini` is used: live `edvr.ini` SHA-256 remains
`C09ED2FB598BDEE1CD14AED5638EC034236E289242CFAF640264F691713DA684`.
The promotion log is `build/plugin-architecture-phase1-evidence-promotion.log`.
These are locally installed test DLLs; the earlier full build's self-contained
installers were not rebuilt by promotion and are not a clean-version release.

Next requested capture: with the same Pimax OpenXR render size, stay stationary
in the carrier cockpit with AA/NV off for 120 seconds, press NumLock, then
DLSS/NV on for 30 seconds and another NumLock. Exit Elite afterward. This
checks production v2 fact wiring, the owner-thread NV sampling guard and the
timed-draw denominator. It cannot normalize the old baseline retroactively or
pass the whole performance gate by itself. Pin flight reads to `03049f8d`;
a following documentation-only commit does not change the installed DLL stamp.

### Production predicate capture and main integration, 2026-10-02

The exact graphics log `edvr_gfx_20261002_163955.log` and paired runtime
`edvr_openxr_20261002_163957_464_57928.log` both verify `03049f8d`, product
version `v0.18.0-7-g03049f8d`, graphics PE stamp `6ABFB378`. The environment
remains Pimax Crystal Super / Pimax OpenXR, 90 Hz, 4032x3898 per eye.
Sean corrected the flight plan afterward: NV stayed off and DLSS ran for
two minutes. Do not describe either capture as NV-on evidence.

NumLock completed two schema-v2 owner-frame sidecars: frame 27806 has
3,831 draws, 125,734 sites and 11,505 actions; frame 40494 has 3,583 draws,
114,886 sites and 11,208 actions. Independent replay reports 6,821 and
6,237 supported selector matches respectively, with zero unknown inputs,
mismatches or unobserved mutations. Every captured draw-gate fact is known
and enabled; eye-range facts cover known-empty ranges and zero counter delta
on the VR-eye route. This verifies production fact wiring for these cases,
not range-hit or disabled-gate flight coverage. Whole-ladder predicate
equivalence remains false. The runtime exits cleanly with owner joined,
cleanup complete, callback retired, retained=0 and exception=0.

Steady AA-off hook windows measure 0.270-0.403 ms per sampled frame and
0.114-0.123 microseconds per timed draw across 3,916-6,002 timed samples.
Steady DLSS windows measure 1.124-1.364 ms and 0.307-0.353 microseconds
across 6,130-7,110 samples. Transition and manual-capture windows are excluded.
The new denominator exposes different draw workloads; the old baseline has
no denominator and these modes cannot isolate an AA cost or pass the
improvement gate. Direct GPU coverage remains partial (DLSS 8/14 temporal
scopes). No Target/RemLok/NV API owner rows or wrapped draws were observed.
Site 50 is NotEligible throughout both eye-route captures. In particular,
the new NV owner-thread/context guard still needs an actual NV-on flight.
Latest stationary-view and visual confirmation remain unreported.

Sean requested merging main into this branch when appropriate. Fetched
main is `743c5dc0`; its 48 commits are integrated for validation. The sole
text conflict was adjacent `plugin_cost.h` and `slow_test.h` includes,
resolved by preserving both. Luna and the independent reviewer identified
five newly documented keys requiring manifest ownership: Core owns
`advanced.input_gate`; Diagnostics owns `advanced.slow_test_ms`; Temporal AA
owns `advanced.flat_cb_map_cache`, `experimental.temporal_aa_engine_motion`
and `experimental.flat_per_draw_lean` in its flat adapter. Catalog validation
now covers 237 keys across nine plugins. Defaults and runtime behavior are
unchanged by those assignments. Main's Map/Unmap and runtime instrumentation
changes make this flight evidence specific to the pre-merge binary; a full
merged-source build and later version-verified flight are separate gates.
The merge goes only into `codex/plugin-architecture`, with no shipping merge
to main. All 126 validation jobs pass (120 pool jobs, six quiet), as do both
production profiles, the 237-key contract and both self-contained installer
checks. Receipt input SHA-256 is
`a084fdad7453beb738eaff531625f4150f9f55c66e90324a66ca7aa9e89d6a94`;
the log is `build/plugin-main-merge-full-build.log`. No merged build is
installed yet; Steam remains on the validated `03049f8d` test candidate.

### NV source facts and selector diagnostic, 2026-10-02

The next schema-2 writer uses predicate-fact version 2 and adds site 50 to
the two existing families. It records raw configured mode, the consumed
callback mode/failure state, draw shape and the existing cached shader
hashes. The independent reader freezes 14a's X/240/1 shape and literal
VS `FCF7BD2896751D96` / PS `F786D34B5E118D5E` predicate. Candidate and active
masks are consistency checks. Current NotEligible/Declined event staging
is evaluated separately from 14a's boolean claim; 14a emitted no site events.
A cached hash of zero is a determinate mismatch of that predicate, not a
statement about the physical shader. Dispatch off with a matching pair and
unavailable raw mode stays unreplayable. Whole-ladder equivalence stays false.

The optional observed callback and mode getter are appended to the private
C-compatible ops record. The ordinary callback offsets are pinned by a
legacy-prefix test. Capture uses one initial fact and one completion;
missing, duplicate, unfinished and malformed facts invalidate the recording.
No new D3D queries, clocks or helper re-evaluation are added. The unarmed
NoTrace/NoCpu classifier matches the saved reference exactly: 7,057 encoded
bytes, 94 calls, 288-byte stack reservation and 1,636 instruction-offset rows,
SHA-256 `2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`.
This is classifier proof only, not whole-hook or whole-module cost proof.

Focused guarded validation passes 264,422 actual NV/WARP checks, including
ordinary/observed claim parity in every mode and failed-mode behavior; 466
registry checks; and the strict 44-draw writer matrix with 67 supported facts,
18 of them NV, with zero mismatches or unavailable inputs. Python reader and
flight-reader self-tests pass. The two actual `03049f8d` sidecars still pass
their original two-family gate and explicitly report NV unavailable in fact
version 1. Review caught an unintended unregistered-module getter dependency
(isolated rig LNK2019) and inconsistent fixture candidate/stage fields; the
corrected getter returns unavailable for absent ops, and explicit fixture
inputs now agree with their recorded shader-gate and shape-miss paths.

`draw_selector_cost_test` exercises the actual typed ladder with synthetic
POD inputs, NoTrace/NoCpu, fixed 20-pair ABBA timing and an A/A noise control.
Its functional checks require equal terminal verdicts and zero gated legacy
handler invocations for all-off interests. The control forces those handlers
eligible; its input-dependent decline work is synthetic. NV is a miss in the
timed corpus. The action stream is deliberately empty. Numeric timing never
fails the build and cannot replace old-classifier parity, module attribution
or a flight performance comparison. It is a separate quiet runner label.

The normal full build passes all 128 jobs: 121 pool jobs in 152.6 seconds
and seven quiet jobs in 26.1 seconds, both production profiles, the 237-key
contract and both installer checks. The log is
`build/plugin-nv-facts-full-build.log`. Its full-pass receipt at
`2026-10-02T23:55:10.586855+00:00` fingerprints source inputs
`98b5435dc7d197a3e91a77d53c0e018e4623de0b6adbfe20e3b2bccda422f76a`.
A production NV-on capture must show an independently replayed positive
claim and actual API owner rows before the NV instrumentation flight gate
closes. Repeatable CPU improvement, GPU non-regression and remaining
predicate families stay open.

The validated source is committed and pushed as `4350a281`. Receipt-guarded
DLL promotion passes at clean version `v0.18.1-15-g4350a281`; the authorized
Steam install and a separate payload/profile verification pass. The personal
INI remains SHA-256
`C09ED2FB598BDEE1CD14AED5638EC034236E289242CFAF640264F691713DA684`.
The elevated install resolved `c:\steam`, whereas the ordinary Steam alias
resolved `C:\Steam`. Startup-config byte comparison rejects that casing
difference; verification with the exact installed target spelling passes.
No config was edited to compensate. The installer built by the full build
retains its earlier dirty version; the clean promotion prepares test DLLs.

The requested next flight is a stationary carrier cockpit view, Pimax OpenXR
at 4032x3898, DLSS on and game NV visibly on for 90 seconds, then one NumLock
capture and exit. Read its logs against `4350a281`, not a later docs-only HEAD.
Its purpose is the positive NV predicate and render-thread API guard evidence;
it cannot by itself establish whole-ladder parity or a performance baseline.

### Positive NV production capture, 2026-10-02

Sean completed the requested NV-on hold and capture. The sanctioned reader
verifies `edvr_gfx_20261002_180854.log` as clean `4350a281`, PE `6AC045AC`,
and pairs `edvr_openxr_20261002_180855_933_42728.log` at the same version.
Pimax Crystal Super / Pimax OpenXR is 90 Hz, EDVR output 4032x3898 and NV
input 2016x1949, DLSS preset K. The loaded DLSS version remains unreported.
The runtime closes cleanly: owner joined, cleanup complete, retained zero,
exception zero; Elite is no longer running.

Frame 25492 contains 3,590 draws, 112,926 site events and 11,305 actions.
Fact version 2 replays all 8,682 supported facts with zero unavailable inputs,
mismatches or unobserved mutations. Its 2,546 NV facts comprise two claims,
2,544 NotEligible and zero declines. Both claims match the frozen literal
shader pair and X/240/1 shape; callback mode is 2 and failed is false. Mode 2
means stock appearance with pulse stability on, not experimental realistic
appearance. Candidate and active masks agree with independently derived
selectors; neither supplies the selector oracle. Whole-ladder equivalence
remains false.

NV engages at 18:12:06.477. Stable API windows ending 18:12:48.794,
18:13:09.982 and 18:13:31.173 report four state and eighteen read/query calls
per sampled frame for cockpit-visuals, across ten annotated call-site IDs.
This is positive owner-level sampling evidence consistent with two pulse-only
NV draws. The log gives the number of IDs, not their mask; it does not directly
separate NV from TargetSharp/RemLok sites. Source and WARP evidence cover that
wiring, and the next reporting slice will publish the existing mask words.

Those windows show partial cockpit classifier CPU at 0.308-0.327 ms per
sampled frame. Draw-hook CPU is 1.279-1.299 ms per sampled frame, or roughly
0.333-0.362 microseconds per timed draw. This is a single flight, not a
matched baseline or repeatability pass. The old baseline has no timed-draw
denominator, so it cannot retroactively supply this comparison. Latest visual
confirmation is pending; the earlier verified baseline reproduces the NV blur.

Next Phase 1 slices run in parallel: site 6 source facts, completed-window API
mask reporting, and bounded Exposure damper API attribution. The site 6 trace
must observe the actual fallback VS hash when the binding shadow is absent or
zero; repeating its query or treating cached zero as a known miss is invalid.
Exposure coverage requires an actual-production-path rig before acceptance.
No Phase 2 migration starts until the open replay and performance gates pass.

### Stars source facts and bounded API coverage, 2026-10-02

Predicate-fact version 3 adds Witchspace Stars site 6. Its independent reader
uses the frozen `14a7ff70` hidden flag, context, X/N shape, count/instances and
consumed VS hash `9AEC596A2B036EA6`. Interest masks only check staging
consistency. The armed helper reports its existing binding-shadow or
VSGetShader fallback input without repeating the query. A staged-out helper can
prove cheap false cases; a missing fallback input stays unavailable. The
observed skipped-counter delta must agree with the independent selector. The
ordinary helper and its call path retain their previous behavior.

The focused writer/reader matrix passes 44 draws and 95 supported matches,
including 28 Stars and 18 NV facts, with no unavailable inputs, mismatches or
unobserved mutations. Initial failures caught fixture defects: NV defaults
overwrote the Stars hash, the expected terminal order omitted the Stars exit,
and the VR-None fixture omitted its cold NotEligible fact. These were repaired
without changing the production selector or weakening the reader. The actual
ordinary and traced Stars helpers pass 116 parity checks, including fallback
query/lookup/release counts and exactly-once counter changes. Version 1/2
historical captures still replay; they provide no production site 6 proof.

Exposure attribution covers only the damper's six direct-context API sites, IDs
96-101. Notes count issued calls, including a failed Map attempt, rather than
eligible paths. A WARP rig runs the actual production function and State
through a narrow test-only compile seam. Its 31 checks cover declines, readback
creation/copy, Map success/failure, Unmap, both writes and sampling guards.
This verifies attribution; it does not test damping numerics or complete
Exposure coverage. Completed-window reports now publish both existing API mask
words, and the collector rig checks a high-word ID.

The guarded NoTrace/NoCpu body remains byte-identical to the saved reference:
7,057 bytes, 94 calls, 288-byte stack, SHA-256
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`. This bounds
the ordinary-path change; it is not a performance comparison with `14a7ff70`.
Full validation passes. Its first attempt compiled the production DLLs,
then the import safety scanner rejected the Stars rig's unneeded
`/NODEFAULTLIB:d3d11.lib` token. The safety gate stays intact. Focused native
runs overlapped despite their build locks; the coordinator serialized the
remaining native checks before starting full validation.

The normal build passed 123 pooled jobs (153.7 seconds) and seven quiet jobs
(26.4 seconds), both production profiles, both self-contained installers and
the 237-key config contract. FocusWatch saw no shown window, opened console or
foreground change. Receipt UTC is `2026-10-03T01:15:19.034091+00:00`, input
SHA-256 `d06382c4056f60fbad234330ab5aeebbc7e43d976ef456d8015600c47654dc7d`;
log: `build/plugin-stars-cost-full-build.log`. Steam remains on the verified
`4350a281` flight build. No new production capture or performance win is claimed.

### Offscreen predicate source facts, 2026-10-02

Predicate-fact version 4 adds offscreen census skip (site 24) and quad skip
(site 26). Both independently replay frozen `14a7ff70` raw configuration and
existing RTV probe results. Census rules preserve target size, optional draw
kind/count and configured order; KIND:0 remains valid. Quad selection preserves
its armed flag, prior eye draw count, literal threshold 100, draw shape and
target size. Probe failure is a known decline; a skipped probe carries unknown
resolution fields. No extra D3D query, clock or allocation supplies these
facts. The four-fact cap remains sufficient because offscreen and eye routes
cannot co-occur: common sites 3/6 precede either 24/26 or 49/50.

Integration checks caught shared-field and rule-name disagreements between
writer and reader, a wrong boolean mutation expectation, and a matrix terminal
that modeled site 24 as Claimed. An attempted reader change to match that
fixture was rejected and reverted: the production site returns Exited=4,
Skip=2, subsite 0. The fixture was corrected, and a higher-tier agent
independently reviewed the frozen source, schema and oracle before rerunning
validation. Site 26 remains Claimed=3, QuadSkip=15. No selector or strict gate
was weakened.

The corrected matrix has 44 draws and 101 supported matches, including census
4/4 and quad 2/2, with no unavailable inputs, mismatches or unobserved
mutations. The armed-production pure helpers pass 19 literal-vector checks,
including KIND:0, ordered matches and the 99/100 boundary. Their probe-count
wrapper is synthetic; it does not spy on production COM calls. Strict
missing/unfinished writer fixtures, dry-run filesystem checks and CLI
rejection/cleanup pass. Pinned production v1/v2 captures still replay; v3
compatibility has synthetic coverage only. No production v4 capture or
forwarding-equivalence claim is made.

The initial pure-helper extraction changed ordinary classifier code generation
by one byte despite retaining its 94 calls. Restoring the two original NoTrace
bodies removes that measurement confound. Fresh guarded NoTrace/NoCpu assembly
again has 7,057 bytes, 1,636 listing records, 94 ordered calls and a 288-byte
stack. Byte SHA-256 is
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`; ordered
call-target SHA-256 is
`e14ff11cac25c3cc7d00d08da6d3423512d2280c6d78e00c8d58dfa66c792f38`. Focus
guards are clean. Full validation passes; Steam remains on `4350a281`.

The normal build passed 124 pooled jobs (144.7 seconds) and seven quiet jobs
(26.7 seconds), both production profiles and installers, and the 237-key config
contract. FocusWatch saw no shown window, console or foreground change. Receipt
UTC is `2026-10-03T01:55:23.307215+00:00`, input SHA-256
`0029289deeee1a848abc8faaf4fa63881c6c347bd1c3f45714e065ab7fcd1675`;
log: `build/plugin-offscreen-facts-full-build.log`. Remaining families,
independent forwarding inputs, all-module cost coverage and matched performance
comparison still gate Phase 2. A larger supported slice is being prepared
before another headset capture.

### Main integration, 2026-10-02

Main `9361bc0d` is integrated into the feature branch at Sean's request; the
feature branch remains unmerged to main. Upstream permanently keeps exact
object motion and lazy draw-record construction and retires the two
experimental measurement switches `experimental.temporal_aa_engine_motion` and
`experimental.flat_per_draw_lean`. The temporal-aa manifest drops only those
retired ownership entries; all nine plugins own the remaining 235 config keys.
Historical measurements above retain their original source/config context. Live
Steam settings and its installed `4350a281` build are unchanged.

The first full attempt failed the unchanged terrain-checkerboard T5c
config-reload assertion. An isolated guarded rerun passed all 188 checks and
102 mutations without a source edit. Its replacement return is unchecked and
failure output lacks observed version/state/toast details, so the first
failure's cause is unresolved; no terrain behavior fix is inferred. The full
retry passed 124 pooled jobs (141.7 seconds) and seven quiet jobs (26.6
seconds), both production profiles and installers, and the 235-key contract.
FocusWatch recorded no shown window, console or foreground change. Receipt UTC
is `2026-10-03T02:24:14.451545+00:00`, input SHA-256
`796fae12729e33074ccd34145810b793e1f98cb614c80505ab641fdb1d57d9db`; log:
`build/plugin-main-9361-retry-full-build.log`.

Future matched performance comparisons need a `9361bc0d` control with symmetric
timed-draw instrumentation. The older control and the latest NV source-fact
capture cannot establish this comparison. Phase 1 replay, forwarding and
complete cost gates remain open; Holo/Scrim source-fact work is next. No Phase
2 or shipping authorization follows from this merge.

### Holo and Scrim source facts, 2026-10-02

Predicate-fact version 5 adds Holo (site 53, kind 7) and Scrim (site 55, kind
8). The eye path now permits six facts: common 3/6, eye 49/50 and Holo/Scrim.
Offscreen 24/26 remain an alternate route. Both selectors independently replay
frozen `14a7ff70` enabled flags, draw shape and resource descriptors. Holo
preserves pattern then depth resolution and the exact lazy eye/render-size
comparison, including the two-pixel allowance and zero height acceptance where
the original permits it. Its counter and noted flag are captured before and
after every outcome, including early declines. Scrim preserves wash then UI
resolution and the existing cache. A separate trace-only descriptor shadow
reuses an already-issued resolve by view/generation; a warm cache without raw
shadow remains unavailable. No additional D3D query, clock or draw allocation
supplies these facts.

Review escalated the failed Luna reader check to the next tier: an unrelated
historical observer fixture needed an explicit v4 version. Further review
rejected missing inputs for a reached Holo eye-size test and allowed
structurally valid UI observations after a raw-unavailable Scrim wash while
keeping that selector unreplayable. Expected selectors use raw inputs; observed
shape/result fields and cached match booleans never replace them. Native
acceptance corrected a lazy-read mask, two fixture member names, frame 14's
omitted source facts and Scrim fixture counts from 240 to 120, preserving the
existing NV shape-miss prefix. No production predicate or strict coverage gate
was weakened.

The actual Holo helper rig passes with synthetic binding-resolver/state seams
and an independent frozen eye-size reference; the observed accessor uses the
production pure selector with lazy getters. The Scrim WARP rig passes actual
helper/cache resolve checks, including fresh failures and retry, raw-shadow
reuse and unavailable warm hits. These do not establish whole forwarding or
production v5 capture coverage. The C++ matrix has 44 draws, 685 sites and 102
actions: 129 supported matches, Holo 15/15, Scrim 13/13 and NV 18/18, with no
unavailable facts, mismatches or unobserved mutations. Frame 14 has six
matches. Missing/unfinished, wrong-site/kind, duplicate and seventh-fact
overflow fixtures are rejected, with explicit CLI checks and cleanup in the
build.

Ordinary Holo/Scrim helpers are unchanged against the frozen source. Guarded
NoTrace/NoCpu assembly still has 7,057 bytes, 1,636 listing records, 94 ordered
calls and a 288-byte stack; its byte and call-target hashes exactly match the
offscreen entry above. Pinned actual v1/v2 captures retain 8,682 and
6,821/6,237 matches with no mismatches, unavailable supported facts or
unobserved mutations. Holo/Scrim correctly remain unavailable in older formats;
synthetic v3-v5 compatibility is covered.

MSVC x64 sizing measures PredicateFact 216 to 376 bytes and DrawRecord 2,472 to
3,864 bytes. With 65,536 records and the unchanged identity table, enabled
replay diagnostics reserve 157.5 to 244.5 MiB, an 87 MiB increase excluding
allocator overhead. Default-disabled configuration allocates no capture buffer;
enabling allocates during initialization, and disabling releases it. This
bounded diagnostic cost is recorded rather than attributed to an unselected
plugin or to per-draw allocation. Steam remains on `4350a281`; no new flight is
requested for this individual slice. Whole-ladder replay, independent
forwarding, complete cost coverage and matched performance remain open before
Phase 2.

The normal build passed 125 pooled jobs (148.5 seconds) and seven quiet jobs
(27.9 seconds), both production profiles and installers, and the 235-key
contract. All 19 expected-invalid CLI checks pass; explicit fixture cleanup
reports no nonempty-directory warning. FocusWatch recorded no shown window,
console or foreground change. Receipt UTC is
`2026-10-03T03:10:41.956163+00:00`, input SHA-256
`635cc08e5af6889770838de7a5dfdd12ff6b47d0c3dfa046c607468c06468889`; log:
`build/plugin-holo-scrim-full-build.log`. The validated source tree is
committed before any later install-only promotion. No v5 production flight or
whole performance improvement is claimed.

### Sunglare source facts, 2026-10-02 (validated)

Predicate-fact version 6 adds sites 61/62/63, kinds 9/10/11. Expected results
come from frozen `14a7ff70` flags, ordered resource descriptors and raw state,
including lazy wants checks and the post-stamp mode read. The helper's existing
clock read supplies the timestamp; capture adds no query or clock. Site 61
records the actual clamp state around the common reset and selector. Site 62
captures its clear and billboard call; site 63 captures the consumed clamp. The
frozen helper never returns Clamp, so no positive site-63 fixture is invented.
Its decline remains covered. The canonical predicate-facts array permits nine
facts, with three Sunglare facts in a separate bounded cold pool.

Review escalated the Luna reader and actual-helper doubts to the next tier. It
corrected an unavailable wants result being used as an oracle, uncoupled the
later mode read, replaced model-written clamp evidence with actual state reads,
and completed the native fixture prefixes and canonical envelopes. Acceptance
caught undeclared factory names, a duplicate fixture masked by the per-draw
cap, and a link recipe/stub overlap. All were repaired before the full build.
The disabled trace visitor initially changed generated bytes because of an
empty member; moving trace-only reset evidence into the enabled policy restored
exact bytes without weakening the comparison.

The production-linked helper rig passes 367,200 ordinary/observed/frozen cases,
including lazy flag reads, shape bounds, ordered resources and timestamp
boundaries. The terminal matrix replays 155 facts across 45 draws, including 20
Sunglare facts (8/7/5 by kind), with zero supported unavailable facts,
mismatches or unobserved mutations. The full-prefix Sunglare fixture replays
nine facts, three Sunglare. All 24 wired invalid CLI checks pass; the five new
checks cover missing, wrong-kind, duplicate, unfinished and per-draw overflow
facts. Native checks also cover the global cold-pool cap. Actual schema-2
predicate v1/v2 captures retain 6,821/6,237 and 8,682 matches respectively; new
selectors remain unavailable in those older formats.

NoTrace/NoCpu remains exactly 7,057 encoded bytes, 1,636 listing records, 94
ordered calls and a 288-byte stack. Byte SHA-256 remains
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`; call-target
SHA-256 remains
`e14ff11cac25c3cc7d00d08da6d3423512d2280c6d78e00c8d58dfa66c792f38`. MSVC x64
measures DrawRecord 3,864 to 3,872 bytes and SunglareObservation 216 bytes.
Enabled replay reservation grows from 244.5 to 285.5 MiB, an additional 41 MiB
excluding allocator overhead. The disabled path allocates no capture buffers;
enabled buffers are allocated during configuration.

The normal build passed 126 pooled jobs (151.5 seconds) and seven quiet jobs
(29.2 seconds), both production profiles, installers and the 235-key contract.
FocusWatch recorded zero shown windows, consoles or foreground changes over 181
seconds. Receipt UTC is `2026-10-03T04:28:25.183541+00:00`, input SHA-256
`08f8445b5d91373d4c610260a033789ded7e0db93eabeb70c7df6d9fec9fbcfe`; log:
`build/plugin-sunglare-full-build.log`. Steam remains on `4350a281`. No v6
production flight or whole-ladder performance result is claimed. Independent
forwarding, complete cost coverage and the matched comparison against
integrated main `9361bc0d` remain gates before Phase 2. FSS sites 57/58 are the
next source-fact slice; Common1 requires its own broader review.

### FSS panel and reveal source facts, 2026-10-02 (validated)

Predicate-fact version 7 adds sites 57/58, kinds 12/13, to the canonical
predicate-facts array. The combined cap is eleven, with a separate two-fact
per-draw cold pool. Caller and helper flags remain independent raw reads;
body/jump frame reads preserve lazy unsigned subtraction and wrap. Panel
captures actual matched-hash state before and after every invoked helper.
Reveal reads arrival-open independently after recognition and records the
actual uint32 counter mutation only when open. Capture adds no D3D query, clock
or draw allocation; ordinary helpers remain unchanged against frozen
`14a7ff70`.

Both frozen helpers initialize a local hash to zero and ignore the guard's
boolean return. Budget exhaustion or a fault before lookup assignment leaves a
concrete zero decline. Registry absence and null shader also return zero. A
Release fault after a matching assignment still claims. Capture distinguishes
callback/getter/lookup/assignment/release progress; progress, guard result and
post-guard hash are consistency observations, not selector oracles. The
independent reader derives the effective hash from initialization plus the raw
completed assignment.

Root review repaired a missing false context observation, a panel after-state
copied before invocation, and duplicate flag reads. Failed Luna Python checks
escalated to the next tier, which repaired lazy body/jump availability and
added unknown-source, malformed-stage, mutation and output-oracle mutants. The
Luna native draft assumed the wrong COM slot. The next tier replaced it with
SDK-typed vtables; its compile failure escalated again, and the top tier
disabled SDK C++ descriptor helpers only in that C-interface test translation
unit. Further checks corrected a wrapped-age fixture, a missing factory frame
argument, an unreachable FSS row whose earlier Holo would claim, and missing
reader accumulator fields. No predicate or coverage gate was weakened.

The production-linked rig passes 505 checks, including real guard faults and
the post-assignment Release claim. The 45-draw matrix replays 178 facts,
including 23 FSS, with zero unavailable facts, mismatches or unobserved
mutations. Sunglare frame 31 replays eleven facts, two FSS, with zero errors.
All 29 wired malformed CLI captures are rejected; per-draw overflow emits an
invalid sidecar, and the distinct global 131,072-slot cap is checked natively.
Focused cleanup reports no nonempty-directory warning.

The zero-interest frame 14 is explicitly limited: nine matches and two empty
FSS NotEligible payloads, both unavailable, with no mismatch or unobserved
mutation. A skipped event never proves the frozen selector would decline. The
default reader exits 1 for this fixture. Its fixture expectation uses
`--dry-run --expect-unreplayable 2`, requiring that exact count and zero other
errors; malformed captures, wrong counts and incompatible arguments fail.
Complete staged-out FSS replay still needs explicit invocation provenance and
separately named cheap-source facts, plus validated shadow/hash provenance
where the original would query. No successful whole-ladder result is claimed.

NoTrace/NoCpu remains exactly 7,057 bytes, 1,636 listing records, 94 ordered
calls and a 288-byte stack, with unchanged byte and call-target hashes. MSVC
x64 measures PredicateFact 376 bytes unchanged, DrawRecord 3,872 to 3,888
bytes, and FssObservation 296 bytes. The new pool reserves 37 MiB; record
growth adds 1 MiB. Enabled replay reservation grows from 285.5 to 323.5 MiB,
excluding allocator overhead. Default-disabled configuration returns before
allocation. Pinned production predicate v1/v2 captures retain 6,821/6,237 and
8,682 matches with zero supported errors; FSS remains unavailable in those
older formats. Steam remains on `4350a281`.

The first full build stopped after 111 of 134 jobs because Windows could not
find the existing cockpit API batch label. The label was present and unchanged,
with an LF-only ending in a mixed CRLF/LF batch. Canonical CRLF normalization
and a corrected focused-wrapper environment allowed its unchanged 56 checks to
pass. The full build was repeated after that check; no cockpit source was changed.

The full retry passed 127 pooled jobs (154.2 seconds) and seven quiet jobs
(30.6 seconds), both production profiles and installers, the 235-key contract
and all 29 expected-invalid CLI checks. FocusWatch recorded zero shown windows,
consoles or foreground changes over 185 seconds. Receipt UTC is
`2026-10-03T05:31:13.099293+00:00`, input SHA-256
`bf93bad6f4905d69d9d9ab66e19be6c2f5f4c115c7a05c3ccfa3cd816c16c246`; log:
`build/plugin-fss-full-build-retry.log`.

Whole-ladder source coverage, independent forwarding and the matched
performance gate remain open before Phase 2. The next cost slice counts
annotated shared resolver query attempts on the registered owner thread. It
excludes Release and cannot establish which immediate/deferred context supplied
a resource. Configuration, shutdown, frame boundaries and ownership transfer
retain the serialized collector lifecycle contract; worker/runtime and other
module costs remain separate coverage gaps.

### Shared binding resolver query cost, 2026-10-02 (validated)

Sites 112-115 annotate the shared resolver's existing GetResource, GetType,
Buffer GetDesc and Texture2D GetDesc attempts as Core ReadQuery work. One
sampling decision inside each admitted guard checks atomic context
registration, published owner identity and existing TLS before reading frame
flags. It never allocates a thread token. Null arguments and exhausted budgets
bypass it. Releases remain outside the measured set; query order and the
established successful-description result after a Release fault are preserved.

This metric means annotated shared resolver attempts on the registered owner
thread. The resolver has no context argument, so the metric cannot prove
immediate/deferred context identity. Collector lifecycle and owner transfer
remain serialized. The new sampler adds work even in unsampled frames; no zero
CPU overhead or complete module-cost claim is made. Report V1 layout and its
completed-frame denominator are unchanged.

The real WARP resolver rig covers texture and buffer views, unsupported types,
null GetResource, raw resources, injected GetDesc and Release faults with
callback counts, stale pointers, independent fix/probe budgets and their spent
paths. Its completed report contains 38 attempts (16 GetResource, 14 GetType, 2
Buffer GetDesc, 6 Texture2D GetDesc), only mask bits 112-115, and one sampled
API frame. Collector tests also complete a mixed window with two sampled empty
frames, retain both foreign-thread/transfer checks, and compile the new C ABI.
Independent review repaired an inert Release test and a double-close before
native acceptance.

Five affected focused rigs passed. The full build passed 127 pooled jobs (144.9
seconds) and seven quiet jobs (30.6 seconds), both DLL profiles, installer
gates and all 235 config keys. FocusWatch recorded zero shown windows, consoles
or foreground changes in 176 seconds. Log:
build/plugin-binding-cost-full-build.log; receipt input SHA-256
ad11e1e1bfa1d7b1d230b85c03995170221cca602743d7296642ac6ab971d488. Steam remains
on 4350a281; no flight or promotion was requested for this slice. Whole-ladder
replay, independent forwarding, complete costs and the matched CPU/GPU
comparison remain open before Phase 2.


### RemLok source facts and staged FSS probes, 2026-10-03

Predicate-fact version 8 adds one cold kind-14 fact at RemLok predecessor site
51; site 52 derives its scissor claim from the same raw fact. The inputs record
both actual mode reads, the depth-view gate, resolver result/type/size, and the
lazy swap read. A resolver failure is a known negative, while an absent
consumed input stays unavailable. Raw uint32 mode values retain existing
behavior: only Stock and Hide receive their special branches. Successful
resource description followed by a guarded Release fault keeps the established
successful result. No extra resource query, clock or per-draw allocation was
added.

The observed helper records actual match, hidden and pending-eye values before
and after its mutations, including uint32 and uint64 wrapping. Matched-path
before values are taken immediately before their corresponding writes; these
snapshots do not establish cross-draw state continuity through possible COM
reentry. The ordinary helper uses the same templated body with observation
compiled out. The real WARP rig exercises shape, mode, depth,
null/wrong-resource/size paths, parity/swap, frame reset, both wraps,
budget/fault behavior and an actual mode change between reads. Its 43 shared
resolver query attempts are checked in one completed API sample window. The rig
aborts if unrelated scissor sizing is entered; headset tangents, cull guard and
visual scissor sizing remain outside this recognition test.

Version 8 also distinguishes actual FSS helper invocation from a raw staged-out
outer probe. Pure probes consume the existing raw configuration, body/jump
stamps, unsigned ages and mode latch lazily, with no helper execution or state
mutation. A false raw outer predicate independently establishes a decline; a
true predicate remains unavailable because the helper/hash inputs were not
consumed. Older v7 empty NotEligible facts remain unavailable. The recorder
rejects contradictory provenance and helper or mutation progress on a raw
probe. Independent review corrected lazy unknown handling without converting
missing inputs into fabricated counter predictions.

Native recorder fixtures cover positive and declined RemLok outcomes,
missing/duplicate/bad/unvisited facts, the global cold-pool cap and a complete
ordered frame with exactly one unavailable staged-out FSS predicate. The Python
parser preserves versions 1-7. The current reader accepts the three earlier
production sidecars: 6,821 and 6,237 matches from v1, and 8,682 from v2, with
zero mismatches or unobserved mutations. This does not prove the new facts have
production-flight coverage.

The compiled NoTrace/NoCpu parent draw body still has exactly 7,057 encoded
bytes, 1,636 listing records, 94 ordered call targets and a 288-byte stack. It
matches the saved reference byte-for-byte; SHA-256
2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40. DrawRecord
remains 3,888 bytes and FssObservation remains 296 bytes. RemLok adds a
112-byte observation pool of 65,536 entries, exactly 7 MiB. Total enabled
recorder pools become 330.5 MiB, excluding allocator overhead; all remain
opt-in. This is diagnostic capture memory, not a performance improvement claim.

Focused tests pass: RemLok 227 checks, FSS 518 checks and the ordered
recorder/CLI suite, including 33 deliberately invalid sidecars and the
expected-unavailable frame. Independent review and native validation repaired
fixture COM declarations, unused link dependencies, an injected view left bound
across cases and stale version checks. The full build's link-isolation gate
caught the new rig's d3d11.lib import before accepting it; the rig now resolves
System32 explicitly and checks the loaded module path.



The System32-isolated RemLok rig passes 229 checks. The final full validation
passed 128 pooled jobs (147.7 seconds) and seven
quiet jobs (31.6 seconds), both DLL profiles, installer gates and all 235
config keys. FocusWatch recorded zero shown windows, consoles or foreground
changes in 179 seconds. Log: build/plugin-predicate-v8-full-build.log; receipt
input SHA-256 8f86e0d017e74b9c1e8383d528bc9fed5b9ac760e771e327f6c91da1767d5703.
Steam remains on 4350a281 with its settings preserved; no new flight or
promotion was requested for this checkpoint. Fifteen selector sites now have
scoped rig replay coverage. Whole-ladder replay, independent forwarding,
complete owner costs and a matched CPU improvement/GPU non-regression
comparison remain open before Phase 2.

### Context and distance source facts, and Intro API costs, 2026-10-03

Predicate-fact version 9 adds raw facts for owner-context site 2 and
distance-enabled site 67. The context fact records the actual consumed context
identities and glare-clamp reset before and after. Context identities share the
existing resource ordinal domain; null is known zero and unavailable reads
serialize null. The reader compares the identities independently and verifies
the zero reset. The distance fact records the actual consumed enable flag.
Neither fact uses the recorded SiteEvent outcome as its selector input.
Versions 1-8 remain supported; whole-ladder predicate equivalence remains
false.

The genuine visitor rig runs the production visitor and TracePolicy into the
real cold recorder pool, using System32 WARP immediate and deferred contexts.
It covers owner and foreign context paths, distance enabled and disabled, and
all four NoTrace counterparts without emitted facts. The rig links the
completed production object set, replacing only its two macro-test translation
units. A targeted startup review found no explicit file, worker or window
startup before CLI handling; transitive vendor-library initializers were not
exhaustively audited. The actual dry-run and guarded run passed with no focus
events. The rig does not serialize its intentionally partial visit tokens as
complete captures. Real null-context producer coverage and cross-draw state
continuity are not claimed.

Ruled out: pruning the isolated VScreen object with either /Gy or /GL/LTCG,
because each link still had 520 unresolved production dependencies. Using the
completed production dependencies avoids hundreds of service stubs and keeps
the visitor genuine.

Ordered native recorder fixtures cover missing, duplicate, wrong-kind,
known-but-unreached and unvisited BasicDraw facts, the 131,072-entry pool cap
and a complete frame with exactly one unavailable context input. All 38
deliberately invalid sidecars are rejected, while the unavailable input stays
unavailable without a fabricated mutation prediction. The current reader also
accepts the three earlier production sidecars: 6,821 and 6,237 selector matches
from version 1, and 8,682 from version 2, with zero mismatches or unobserved
mutations. This is compatibility evidence, not a production capture of version
9.

The compiled NoTrace/NoCpu parent body still matches the saved reference: 7,057
encoded bytes, 1,636 listing records, 94 ordered call targets and a 288-byte
stack, SHA-256
2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40. BasicDraw
observations are 64 bytes, adding an 8 MiB pool. DrawRecord grows from 3,888 to
3,896 bytes, adding 0.5 MiB, and the identity table adds 1 MiB. Enabled
recorder pools total 340.0 MiB, excluding allocator overhead; default-disabled
capture allocates none of these pools. No extra D3D query, clock or per-draw
allocation was added to these predicates.

Intro-owned SplashDim API sites 83-90 count six direct save/apply/restore state
calls and the cold blend-state GetDevice and CreateBlendState attempts.
ReadQuery, State and Work remain distinct. The active bracket carries its
original sample eligibility through End, while null End and nested Begin
preserve the bracket. Shared resolver queries stay Core-owned at sites 112-115.
Shader-swap helper creation, Releases and the SplashDim CPU bracket remain
outside this bounded cohort.

The independent SplashDim System32-WARP rig passes 5,469 checks: exact cold and
warm attempted counts and masks, completed 1,800-frame denominators including
empty frames, declined and unsampled paths, real state restoration, nested
Begin/null End and a typed real-context PSGetShader fault prefix. The fault
case proves pre-mutation attempt accounting and subsequent recovery; partial
setter/restore faults are not covered.

The full validation passed 130 pooled jobs (152.5 seconds) and seven quiet jobs
(32.6 seconds), both DLL profiles, installer gates and all 235 config keys.
FocusWatch recorded zero shown windows, consoles or foreground changes in 185
seconds. Log: build/plugin-predicate-v9-full-build.log; receipt input SHA-256
9cedf0ee7d5c3b0e7e5e564e994ea8ebb5d7ee7e1be8d32f76f0aafb4d37ae6e. Steam remains
on 4350a281 with settings preserved. Seventeen selector sites now have scoped
rig replay coverage. Whole-ladder replay, independent forwarding, complete
owner costs and a matched CPU improvement/GPU non-regression comparison remain
open before Phase 2.

### Eye-census rules and shared shader costs, 2026-10-03

Predicate-fact version 10 adds one kind-17 source fact at eye-census site 48.
It records the raw initial count, each consumed loop bound and the natural
terminal bound, up to eight rules and four filters per rule. A nonzero VS-hash
rule compares the actual held hash against the later configured hash read and
bypasses count and SRV filters on a match. Count rules preserve the range
upper-bound and kind comparison order. Filters retain separate mode reads
before and after resource resolution, including only the width and height
operands actually consumed. Draw kind and count come from the existing
immutable invocation facts. The independent reader derives Exited/Skip and the
winning rule index, including zero, from those inputs. Recorded outcomes remain
consistency checks.

The two counter snapshots surround the actual uint64 increment, proving its
local modulo wrap without attributing callback mutations to the enclosing draw.
Declines have no counter-write observations. Unknown selector inputs remain
unavailable; a known winner with an unavailable counter snapshot reports
missing mutation evidence. The parser validates every envelope, cardinality,
type and domain before a lazy unknown exit. Null non-None filters still consume
the real resolver return, and skipped hash/filter reads cannot fabricate
progress. Counts above the physical eight-rule capacity invalidate the capture;
the trace's bounded loop does not establish behavior for corrupt or
over-capacity state.

The genuine visitor/System32-WARP rig covers all five filter modes, texture and
structured-buffer resources, exact and ranged counts and their lazy misses,
hash bypass and mismatch, rule seven, eight-rule termination, eye-size
availability and mismatches, None with a present binding, and uint64 wrap.
Typed GetResource callbacks count actual attempts and change later mode,
configured dimensions, loop bounds and counter values. The resulting
observations and persistent effects agree with those changes. Typed faults have
Trace/NoTrace parity, and the macro seam restores prior bindings and shader
identity/hash on success and failure. Each callback scenario uses a fresh hook
because reusing a CopyVptr hook instance retained its mechanism state. No
production hook behavior was changed.

Ordered recorder fixtures add missing, duplicate, bad-kind, bad-envelope,
over-capacity and unvisited facts; a valid capture with exactly one unknown
eye-census input; and independent exact, late-hash wrapping and resource-size
winners. The reader preserves versions 1-9 and whole-ladder equivalence remains
false. Native acceptance caught a reader indentation regression that treated
legacy successful Stars/NV/Holo/Scrim claims as missing mutation evidence. The
unchanged terminal matrix passes with 271 selector matches and zero unavailable
inputs, mismatches or mutation warnings after a scoped correction. Separate
schema-10 regressions pin those four existing positive selectors; no gate was
relaxed.

The opt-in EyeCensus pool has 32,768 entries of 3,256 bytes, exactly 101.75
MiB. Its overflow has a distinct invalidation reason and rejects the complete
capture. DrawRecord grows by eight bytes to 3,904, adding 0.5 MiB. All enabled
recorder pools total 442.25 MiB, excluding allocator overhead, versus 340.0 MiB
at the preceding checkpoint. Default-disabled capture allocates none of them.
The compiled NoTrace/NoCpu parent still matches the saved reference exactly:
7,057 bytes, 1,636 listing records, 94 ordered calls and a 288-byte stack,
SHA-256 2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40. No
extra rendering query or clock was added to the predicate.

Core-owned API sites 116-121 cover shared precompiled VS/CS/PS creation: each
actual GetDevice attempt is ReadQuery, and each actual CreateShader attempt is
Work. Sampling requires the registered owner context/thread and is latched
after the original public-input guards. Notes precede the calls inside the
original admitted fault guard. Releases and dynamic D3DCompile remain outside
this bounded cohort. SplashDim's cold Core report now adds its helper's PS
creation sites 120/121; warm reports add no creation work.

The shared shader System32-WARP rig verifies successful VS/CS/PS and
malformed-bytecode attempts, rejected null inputs, real effects in
unsampled/deferred/worker calls without render-thread charges, and a typed
GetDevice fault followed by recovery. It checks exact counts, masks and
completed 1,800-frame denominators with the production fault guard. The
SplashDim rig still passes 5,469 checks. Shader-creation faults, output
publication or Release faults and exhausted shader budgets remain untested.
Existing standalone shader-swap consumers now link the collector and generated
declarations.

Full validation passed all 138 jobs: 131 pooled jobs in 142.8 seconds and seven
quiet jobs in 35.1 seconds. FocusWatch ran for 178 seconds with zero show,
console or foreground events. Both production profiles, Python self-tests,
native rigs and the self-contained installer passed; the config contract agrees
on all 235 keys. Receipt input SHA-256 is
01acde3591a91aed3020d1a41f30ce0a7fa7120989f0a3d3fa428a48e1aa9c91; log:
build/plugin-predicate-v10-full-build.log. This is a validated Phase 1
checkpoint, not whole-ladder equivalence or a performance acceptance result.

### Resolve-binding inputs and loader staging costs, 2026-10-03

Schema 11 adds raw facts for ungated EyeSequence site 60, ResolveBindClaim.
Each reached site emits one pointer-free kind-18 observation. The selector
records the outer enable, PS presence and PS hash reads, then the helper's
independently repeated enable, context and shadow reads in their consumed
order. Cached positive and negative paths consume no getter fields. Null or
zero-hash shadows remain unknown to the cache and enter the existing guarded
fallback; a completed null shader or zero lookup is a known negative.

Fallback facts record lambda admission, PSGetShader/lookup/Release entry and
completion, the raw lookup hash, and the guarded return. Match starts false;
replay derives its later assignment from this prefix. A Release fault after
assignment preserves a positive claim, while a prior getter fault and denied
budget retain false. Cache presence/hash snapshots bracket only the actual
nonzero-hash binding repair. Both fields are read even when the pointer is
absent, and before/after callbacks impose no invented continuity requirement.
The independent reader was corrected when review found it rejected that
legitimate absent-pointer hash read; producer-shaped and adversarial fixtures
now pin it. Whole-ladder predicate equivalence remains false.

The genuine System32-WARP visitor rig invokes the actual site and production
helper with real registered and unregistered pixel shaders. It verifies outer
off, cached No/Yes without getters, null and zero-hash fallbacks, successful
repair, a typed PSGetShader fault and recovery, Release-fault positive
retention and recovery, and admission denial after the eight-fault budget is
exhausted. The selector harness does not run the separate vertex-buffer lending
actions. Typed callbacks and the real fault guard establish prefixes;
fabricated helper results are not replay inputs. The existing action rig
remains a separate gate.

The cold recorder has a dedicated 32,768-fact pool, mandatory visited-site
coverage and separate overflow/missing-pool invalidation. Native writer
fixtures 68-76 cover missing, duplicate, wrong-kind, malformed reached/known,
unvisited, unknown, capacity, cached/fallback/Release-fault positives and five
negative paths. All strict CLI checks pass: positive fixture 39 selector
matches and negative fixture 85 matches, with no unavailable inputs, mismatches
or mutation warnings; the intentional unknown fixture reports exactly one
unavailable selector. The original terminal matrix and schema 1-10
compatibility remain validated. Unknown is never treated as a default negative.

Native sizeof measurement is 168 bytes per ResolveBind observation, 5.25 MiB
for its pool, and 3,912 bytes per DrawRecord. Enabled replay storage totals
448.0 MiB, excluding allocator overhead; the default disabled path allocates
none. The NoTrace/NoCpu draw listing remains exactly the saved reference: 7,057
bytes, 1,636 listing records, 94 ordered call targets and 288-byte stack. Bytes
SHA-256: 2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40.
Call-target SHA-256:
e14ff11cac25c3cc7d00d08da6d3423512d2280c6d78e00c8d58dfa66c792f38. These
measurements do not establish complete module costs or a flight gain.

Intro-owned API sites 91-110 now count the loader-panel staging lifecycle.
Notes precede actual calls inside the existing guards; one sample latch follows
capture/readback admission. Ten queries are ReadQuery, four CreateBuffer calls
are Work, and four copies plus four Map/Unmap pairs are Transfer. Releases and
classification outside this lifecycle remain excluded. The actual System32 WARP
rig checks a complete 10/4/12 report, a first-Map fault with 10/4/5 attempted
calls and a separate 0/0/8 recovery, a three-query GetDevice fault followed by
successful cold capture, exact owner masks and 1,800 completed sample frames.
Worker staging allocations occur with zero Intro charges. Deferred testing
observes the actual IA getters and distinguishes their admitted prefix from
successful staging. A real command list alone is not staging evidence.

Review corrected the rig's IA getter slots to 79/80 and its fixture lifecycle:
texturing prevents staging capture but does not cancel the original speculative
withhold, and a real disabled tick closes the previous frame before a new
fixture. Production semantics were preserved. The rig uses production COM,
fault guard, capture and analysis; the default-only configuration/log boundary
is excluded from its claim. Map HRESULT/partial-prefix cleanup,
Unmap/CreateBuffer faults and gameplay draw classification remain untested by
this cost cohort. Focused native logs: build/plugin-predicate-v11-focused.log
and build/plugin-loader-v11-focused.log; final loader rig PASS, zero failures.

Full validation passed all 139 jobs: 132 pooled jobs in 146.9 seconds and seven
quiet jobs in 37.7 seconds. FocusWatch ran for 185 seconds with zero show,
console or foreground events. Both production profiles, Python self-tests,
native rigs, the 235-key config contract and self-contained installer checks
passed. Receipt input SHA-256 is
9a78021079084a0bb7909702aa8dfd56433e3adf7a9e04ec6d774f81462d20b6; log:
build/plugin-predicate-v11-full-build.log. This remains Phase 1; independent
forwarding, remaining predicate and owner cost coverage, matched CPU
improvement and direct GPU non-regression remain open.

### Loading-panel inputs and UI state costs, 2026-10-03 (validated)

Predicate fact version 12 adds site 25, OffscreenLoaderPanel. The pointer-free
fact records lazy outer gates, sequence folds and bounded writes, first-panel
and chain state, collection admission and immediate write checkpoints, and the
freshly consumed chain/speculative-withhold inputs after collection. The reader
derives the claim from those inputs; no helper return or category boolean
supplies the answer. It preserves unsigned folds, ordinal and drop wraparound,
signed base vertex, and the sentinel that suppresses a chain scan. Opaque
collection work still carries a mutation warning even when narrow scalar writes
replay. Resource capture, learned-cache history and cross-draw state provenance
remain unresolved; whole-ladder equivalence stays false.

The genuine System32-WARP visitor rig verifies first/second speculative panels,
lazy target and draw-shape refusals, sequence positions 47/48, the fourth chain
entry, ordinal wrap, textured capture exclusion, and real typed
IAGetIndexBuffer callback re-entry and SEH. Successful re-entry changes the
capture index before its write; the fact records the immediate 23-to-24 write
and later classification flags. Fault re-entry verifies drop-counter wrap. Six
actual callback faults exhaust the production budget; the next visit skips the
getter, records its drop increment, and has no opaque-work warning. NoTrace
also exercises a real fault with zero observations. The trace-only physical
chain bound retains invalid raw input without indexing past four slots; the
NoTrace specialization excludes that diagnostic branch.

Native writer fixtures 77-86 cover missing, duplicate, wrong-kind, malformed,
unvisited, unknown, over-capacity, global pool overflow, both selector paths
and an explicit collection warning. Strict CLI checks pass: the positive and
negative fixture has 11 selector matches, zero unavailable inputs, mismatches
or warnings. The collection fixture has five known matches and exactly one
warning. Unknown remains unavailable. Empty/bypass captures and schema 1-11
compatibility pass. Review repaired lazy-height validation, callback write
admission, exact UINT arithmetic and empty-capture metric initialization.

Native sizeof measurement is 420 bytes per LoaderPanel observation, a 13.125
MiB pool, and unchanged 3,912-byte DrawRecords. Enabled replay storage totals
461.125 MiB excluding allocator overhead; disabled allocates none. The compiled
NoTrace/NoCpu draw body remains byte-identical to the saved reference: 7,057
bytes, 1,636 records, 94 ordered calls and a 288-byte stack. Bytes SHA-256:
2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40.

Core-owned API sites 83-96 count the UI layer state save/apply/restore
transaction at real getter and direct/raw setter boundaries. The sample latch
follows Begin admission and survives cleanup before being cleared. The whole
production UI layer WARP rig passes 641 checks, including exact 6 ReadQuery/8
State totals, coverage mask 0x1FFF80000, 1,800 completed sample frames, state
restoration, declined/unsampled paths, worker/deferred exclusion, a typed
third-getter fault with 3 ReadQuery/0 State attempts, and recovery through the
existing configuration rearm. Shared shader, depth-seeding, clear and cold
blend-cache work stay outside this bounded cohort. The rig uses the real guard
and WARP multithread protection.

Full validation passed 139 jobs: 132 pooled in 147.2 seconds and seven quiet in
40.2 seconds. FocusWatch observed zero show, console or foreground events over
187 seconds. Both production profiles, Python/native gates, 235 config keys and
the self-contained installer passed. Receipt input SHA-256:
99942fa35be3d7772c9316c138cb4b11dd7b68b0aa1fde8d241fc3f4bc31ebba. Logs:
build/plugin-predicate-v12-full-build.log and the v12 focused,
continuation-focused and UI-focused logs. Steam remains on verified 4350a281;
this checkpoint was not installed. Remaining source predicates, independent
forwarding, complete owner costs and matched CPU/GPU performance remain open
before Phase 2.

### FSS dump source replay, 2026-10-03 (validated)

Predicate fact version 13 adds site 59, FssDumpClaim, as the twenty-first
supported selector. The producer records both independent lazy wants
expressions, the outer frame window, and the actual shader-query/guard prefix.
Interest admission is derived from site 6's raw mask, rather than the handler
marker. A reached NotEligible event carries unused source reads; the reader
derives its production outcome independently.

The reader derives ring, composite and tonemap families from the consumed hash
and draw shape. It checks the selected byte counter's increment and pending
kind/eye writes against before/after inputs. It preserves existing unsigned
behavior: counter 255 increments to zero, allowing pending eye UINT_MAX. Null
shader lookup is still reached and returns zero; a Release fault preserves a
hash already assigned. Guard return is a consistency check because the
production helper ignores it. Unknown mutation inputs remain unavailable and
warn where writes could have happened. These facts do not establish cache
provenance or prior counter history.

The actual WARP harness compares the trace helper against the unchanged
original helper from identical seeded scalar state. It covers all three
families, counters 1/2/3/255, dumping off, lazy/window declines, unsigned frame
wrap, null shader lookup, an assigned-hash Release fault, and natural
exhaustion of the eight-fault budget. It does not invoke capture Begin/End for
the UINT_MAX eye case. Registered shader metadata supplies current lookup
inputs; this is not proof of production learner history.

Native serialization and reader gates cover source-free mask exclusion,
positive/negative selectors, counter wrap, missing/duplicate/unvisited facts,
bad read domains, unknown inputs and pool exhaustion. The Python self-test pins
the literal NotEligible production tuple, so comparing two copies of the same
incorrect expected outcome cannot pass that check. The final ordered native run
has zero mismatches and zero unavailable inputs in the known positive/negative
and wrap fixtures.

The fresh MSVC footprint probe measures a 200-byte FSS dump observation and a
32,768-entry pool (6.25 MiB). DrawRecord grows from 3,912 to 3,920 bytes (0.5
MiB across the draw capacity). Total enabled replay storage is 467.875 MiB
excluding allocator overhead; disabled allocates none. The compiled NoTrace
ladder still has the same 7,057 bytes, 1,636 instruction records, 94 ordered
calls and 288-byte stack frame as the saved reference.

Full validation passes all 139 jobs, including both production profiles, Python
self-tests, actual native rigs and the self-contained installer. The log is
build/plugin-predicate-v13-full-build.log. FocusWatch reports zero shown
windows, consoles or foreground changes. This is a Phase 1 checkpoint:
independent forwarding, complete owner cost coverage and the matched CPU
improvement/GPU non-regression gates remain open. Steam continues to use
verified 4350a281; no new flight was spent on this slice.

### Forwarding inputs v1, 2026-10-03 (validated)

Predicate facts remain at version 13. A separate `forwardInputVersion: 1`
records raw inputs consumed by `forwardWithVerdict`, including its verdict
parameter, repeated lazy UI/issue gates, captured depth flags, raw engine cache
family, actual callback result and the post-callback reads. The pointer-free
observation reuses site 2's normalized context identities and `DrawFacts`; no
extra context query or identity slots were added.

The reader independently derives the local original/skip action plan for
owner-context `None` and `Skip` draws with every relevant side-work gate known
false. It preserves short circuits, separate repeated reads and signed source
domains. A skip that actually clears an armed curve flag is unavailable because
that mutation is outside this slice. Other verdicts, foreign contexts, active
or unknown side work, missing entry and missing inputs are explicitly
unavailable. Normalized D/N start vertices use the source's unsigned cast,
including `INT_MIN` and `-1`; indexed base vertices remain signed. Recorded
actions and forwarding facts are comparisons, never inputs to the expected
plan.

This is a local forwarder check. The real callback can perform additional work,
including the X thunk's motion reissues. Its actual returned boolean is an
external input; callback-internal D3D work is unobserved. The report keeps
terminal proof unavailable and whole forwarding equivalence false.

The production WARP harness calls the actual forwarder with an injected
callback and compares Trace/NoTrace callback count, classification and selected
scalar state. It covers None/Skip, blocked entry, active ledger, callback
failure, repeated post-callback gates and lazy planet OR reads. Exact typed
action tuples cover D/I/N/X and literal unsigned edge cases; NoTrace records no
observations. Native canonical writer/reader fixtures cover applied/declined
originals, skip, missing entry, unknown input, duplicate/malformed append and
cold-pool overflow. Forwarding count CLI assertions must also pass the existing
predicate gate; regressions reject predicate mismatches, unavailable facts and
mutation warnings without any filesystem writes. The new ordinary fixtures
record a real NV shape hit and a consumed failed-mode state, rather than
contradicting `DrawFacts`.

Review caught 14 additional NoTrace calls caused by an empty nontrivial
observer destructor. A trivially destructible specialization removes that
cleanup scope. The original draw-ladder visitor remains byte-identical: 7,057
bytes, 1,636 records, 94 ordered calls and a 288-byte stack allocation. The
eight forwarder bodies are not byte-identical: each is 2,782 bytes and 756
records versus 2,790/758, with the same 71 ordered calls and 176-byte stack
allocation. Inspection traced the reduction to two redundant zero extensions
(seven bytes) and one padding byte. Direct packing in `edx` already clears its
upper bits. There are no additional calls, reads or branches in this compiled
comparison; it does not establish flight timing.

The fresh MSVC footprint probe measures a 72-byte forwarding observation,
65,536 slots (4.5 MiB) and DrawRecord growth from 3,920 to 3,928 bytes (0.5
MiB). Enabled replay storage totals 472.875 MiB, excluding allocator overhead.
The disabled default still allocates no replay pools. Append marks entry before
validation, and missing-pool/overflow failures invalidate the capture rather
than masquerading as an unvisited forwarder.

The focused production/WARP, ordered canonical replay, NoTrace compilation and
footprint gates passed in 34 seconds with no window shown, console opened or
foreground move. The full build passed all 139 jobs: 132 pooled jobs in 154.8
seconds and seven quiet jobs in 45.2 seconds. Both production profiles, the
235-key config contract and the self-contained installer passed. FocusWatch
observed no shown window, opened console or foreground move to the build tree
in 200 seconds. The source receipt is
`679dd5d4b9f1a3a723127ca6dc606c7d78dd2ab87ee4820f90c4b96be6586cd2`
(`v0.18.1-31-g4a624a92-dirty`).

Legacy UI/intro and on-foot maps source-order pins were updated to the
observed-read expressions. The checks retain the original recognition, refusal,
curve/strip and reissue order. The focused quality rig passes 3,218 checks, the
maps rig passes 80 checks, and all 50 deliberately broken wiring variants are
caught by named checks. Mutation anchor self-tests pass for all 96 world-route
and 53 maps variants. No Steam build was installed and no additional flight was
requested. Remaining predicate/forwarding/state coverage, all-module costs and
matched CPU/GPU performance remain open before any Phase 2 migration.

### TargetSharp source facts and FSS dump costs, 2026-10-03 (validated)

Predicate fact version 14 adds TargetSharp site 54, the twenty-second supported
selector. Its typed observation records the independently consumed outer and
helper sharp/failed flags, actual SRV0 presence/resolution/type/dimensions, the
existing lazy eye-size helper reads, short-circuit SRV1-3 presence, actual
VSGetShader presence, registry lookup hash and configured private shader pin.
No cached match or recorded verdict supplies the expected selector. Hash zero
remains a known source value. Admission is independently derived from site 6's
raw interest bit 0; a reached NotEligible visit records one default observation
with no handler or source reads. This does not add whole-ladder equivalence.

The real vScreen WARP rig exercises the actual site, published interest mask,
tracked resource resolver, real SRVs/buffers and shaders, production shader
registry, private configured pin, lazy eye/render dimensions and auxiliary
slots. Trace and NoTrace retain the same outcomes and existing query counts.
The guarded resolver fault remains enabled through both parity calls; no fault
budget is reset. These bounded visits do not substitute for a complete flight.

Canonical writer frames 101-104 cover positive, lazy failed-gate decline,
NotEligible and reached-unknown input; the last is explicitly unreplayable.
Frames 105-110 cover missing/duplicate/malformed/unvisited facts, the per-draw
cap and 32,769 draws against the 32,768 global pool cap. Invalid captures emit
explicit incomplete/truncated/overflow sidecars, matching the existing writer
contract; reader invalidation is checked. Python negative controls reject
malformed read envelopes, skipped eye metadata with consumed fields, a resolved
null SRV0, missing required gate reads and raw-input contradictions. Historical
predicate versions 1-13 remain valid with TargetSharp unavailable. Existing
predicate and forwarding CLI gates remain active.

Fresh compiled comparisons against saved production inputs confirm:

- TargetSharp's public default path and false observation specialization each
  retain 293 bytes, 71 instruction records, seven calls and a 64-byte stack.
  SHA256: 0677240616ffc13379cc6f153c92796842ac1d3a6dc93c5d7cc1a9d838d2614c.
- The NoTrace/NoCpu visitor retains 7,057 bytes, 1,636 records, 94 calls and a
  288-byte stack, with the same saved byte and ordered-call hashes.
- All eight NoTrace forwarding bodies match the validated forwarding-v1
  baseline: 2,782 bytes, 756 records, 71 calls and a 176-byte stack each.

The fresh MSVC footprint probe measures TargetSharpObservation at 120 bytes;
its 32,768-entry cold pool adds 3.75 MiB. Generic PredicateFact remains 376
bytes and DrawRecord remains 3,928 bytes. Total retained payload is 476.625
MiB, excluding allocator overhead; disabled replay allocates none.

FSS dump capture/captureRemembered/writeOut now attribute direct API attempts
to Scanners at stable sites 39-52. Each helper samples the actual owner context
once; notes immediately precede the attempted calls. The real System32 WARP rig
passes 57 checks: cold 8 ReadQuery/2 Work/6 Transfer, warm 6/0/6, exact
coverage masks, read/work/Map failure prefixes, null Map data, state/byte
preservation, and no notes for unsampled/deferred/worker calls. Series capture
and series write, predicate shader queries and owner CPU coverage are outside
this slice. API counts do not establish a performance improvement.

The first full run stopped at the old TargetSharp source pin, which inspected
the new thin wrapper rather than the shared query implementation. The repaired
gate pins sample/note/query order, both wrapper routes, missing-symbol checks
and bypass negative controls. The next full run found the standalone cockpit
rig's missing observed eye bridge. Function packaging and dead-code flags did
not resolve that direct-object dependency; those flags were restored. The
fixture bridge preserves its coarse cutoff, declares production eye metadata
unavailable and asserts zero calls from all default-path cost cases. The real
vScreen rig supplies observed-path coverage; cockpit passes 57 checks.

The final absolute-path full build passed all 140 jobs: 133 pooled jobs in
145.6 seconds and seven quiet jobs in 47.0 seconds. Both production profiles,
Python self-tests, native rigs, the 235-key config contract and self-contained
installer checks passed. FocusWatch recorded no shown window, console or
foreground move to the build tree in 193 seconds (one unrelated raw show
event). Full-build receipt input SHA256:
`e66de417eb067f1f0b780707e6b28f15154b9ba5f2dedbd22fdbaeeefa37008a`. Build
label: `v0.18.1-37-gf2305990-dirty`.

Steam remains on verified 4350a281 with settings preserved. No new build was
installed and no flight was requested for this slice. Whole-ladder predicates,
winning-verdict effects and callback work, all-module API/CPU coverage, matched
CPU gain beyond noise and direct GPU non-regression remain open. The next
bounded source slice is Sunglare nomination site 45; the next default-module
API slice is FSS Reveal Begin/End. Phase 2 and shipping remain gated.

### Sunglare nomination sources and FSS reveal costs, 2026-10-03 (validated)

Predicate fact version 15 adds the unconditional Sunglare nomination
observer at site 45, the twenty-third supported selector. Its raw inputs are
signed world mode, the classification count, actual VS CB0 identity, prior
local nomination identity, guarded resource resolution, buffer type and byte
width. Callback occurrence and the two post-callback identities are checked
as mutation consistency, never used to derive the expected decision. All
four pointer values serialize only as capture-local ordinals, including
contradictory post-callback values. Null is ordinal zero. The reader
enforces each immediate lazy parent even when another input is unknown; an
unknown source remains unavailable. Historical versions 1-14 keep this
observer unavailable.

The System32 WARP rig uses the real visitor, actual bound buffers of
192/208/224 bytes, null binding, count 10000/10001, world zero/negative, and
seeded prior nomination. A real guarded GetType fault remains active through
both Trace and NoTrace calls; no budget is reset. The non-buffer case is
explicitly synthetic shadow state because the actual constant-buffer binding
cannot accept a texture. Local nomination history is seeded per fixture, so
these checks do not prove cross-draw learner continuity. Callback/local
state agree and existing resolver query counts are preserved.

Canonical frames 111-114 cover positive nomination, unresolved lazy decline,
cutoff and unknown world input. Frames 115-120 invalidate missing,
duplicate, malformed, unvisited, per-draw-cap and global-overflow captures.
Full canonical prefixes and existing forwarding-v1 CLI gates remain active.
Negative reader self-tests independently cover malformed lazy inputs. Writer
fault fixtures may also fail canonical-prefix validation; they are not
claimed as independent raw-predicate negative proofs.

Fresh compiled NoTrace/NoCpu comparison retains 7057 bytes, 1636 instruction
records, 94 calls and a 288-byte stack; byte SHA256 is
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`. All
eight NoTrace forwarding bodies retain 2782 bytes, 756 records, 71 calls and
a 176-byte stack against the validated forwarding-v1 baseline. The MSVC size
probe measures a 104-byte observation and a 32768-entry pool of 3.25 MiB.
DrawRecord grows from 3928 to 3936 bytes and the identity pool grows from 4
to 6 MiB. Generic PredicateFact remains 376 bytes. Total retained payload is
482.375 MiB excluding allocator overhead; disabled replay allocates none.

FSS Reveal Begin/End now attribute direct attempted API calls to Scanners at
sites 53-73. Begin samples once after admission; End uses the matching
context's latched decision, with lifecycle clearing. The production
learning/shadow path, guarded budget, rendering and references are
preserved. The real WARP rig passes 64 checks: cold texture capture 9
ReadQuery/2 Work/1 Transfer/2 State; cold CB creation with warm textures
9/1/3/4; fully warm 8/0/3/4. Steady CB capture is cold 3/1/2/2 and warm
2/0/2/2. Exact masks, actual copied pixels and written CB bytes, bound-state
restoration, HRESULT/null failure prefixes, sample/context/thread filtering
and lifecycle cases pass. Shader lookup is a zero-hash boundary shim in this
cost rig; selector and whole-game equivalence are outside its scope.

The cold DLSS creation path now reports the actual already-loaded
`nvngx_dlss.dll` path and mapped RT_VERSION/1 fixed file version, with a
temporary module reference. It never loads an absent runtime, reopens a disk
path for version data, polls on evaluation or retains a reference. Complete
path/version or explicit unavailability is logged after the existing NGX
duration sample and deduplicated. Identical path/version/HMODULE
unload/reload transitions between observations are indistinguishable. The
native rig passes 26 checks using mapped kernel32 resources, a real
no-version fixture, malformed/unaligned resource copies and actual-module
report transitions. `edvr_log.py --version` reports this observation from
the same build-verified graphics log; historical missing records are
unavailable.

Acceptance repairs were confined to tests/build integration: real
buffer/texture GetDesc is vtable slot 10, not 8; ordinary canonical fixtures
explicitly seed known world zero; the frame-14 fixture now includes
nomination input. NewBegin cost testing retains its first sampled frame and
re-enables the collector before End to independently test the new unsampled
latch. An all-unsampled window closes and publishes zero throughout all 1800
frames. The full build exposed the older FSS predicate rig's missing actual
collector link. Generated no-version fixture libraries now stay under
ignored build output.

The final absolute-path full build passed all 142 jobs: 135 pooled jobs in
222.0 seconds and seven quiet jobs in 58.3 seconds. Production profiles,
Python self-tests, native rigs, the 235-key config contract and
self-contained installer checks passed. FocusWatch recorded no shown window,
console or foreground move to the build tree in 280 seconds. Full-build
receipt input SHA256:
`2fd1013e1c1edfea8b1eca1398b0c9291dac9d7764340180829b32a4bec4b5e9`. Build
label: `v0.18.1-38-g6475e38f-dirty`.

Steam remains on verified 4350a281 with settings preserved. No new build is
installed for this checkpoint. Main-based control preparation uses pinned
83938927 and only the timed-draw denominator plus the same mapped-DLSS
reporter; it still needs its own full build and clean promotion. The metric
is hook time minus its first forwarding interval, whose indexed-instanced
interval can include a weapon-motion reissue; it is not total EDVR CPU. The
original frame offset zero and per-thread draw stride are preserved. Section
11 permits scoped NV pilot comparisons; section 8 adds missing feature
fixtures per Phase 2 group. This does not waive Phase 1 acceptance or
authorize Phase 2. Whole-ladder predicates/actions, remaining module API/CPU
coverage, repeatable CPU improvement beyond noise and direct GPU
non-regression remain open. The next useful cost slice is repeated on-foot
panel CB apply/restore; broad temporal and mixed menu ownership need
separate review.

### Main integration and symmetric control preparation, 2026-10-03

Main revision `83938927e3488b693cb85e3375541f6b3e5bc0b6` is integrated into
the plugin branch after checkpoint `67e26af7`. The source delta is flat-mode
capture diagnostics. All seven draw-hook conflicts preserve feature
trace/action records while main's footprint begin/end calls bracket the real
flat draw, including indirect buffer/offset arguments. The build conflict
retains both sides' gates. This integration does not merge the plugin branch
into main.

The combined source passes all 142 full validation jobs: 135 pooled jobs in
168.4 seconds and seven quiet jobs in 50.4 seconds. Production profiles,
Python self-tests, native rigs, the 235-key config contract and
self-contained installer checks pass; FocusWatch reports no shown window,
console or foreground move to the build tree in 219 seconds. Receipt input
SHA256: `ab027b6b4466114903cf2fe95e0c27ed609941a9c04ff168032bcc32b7db61fd`.
Fresh merged-source assembly retains the prior 7057-byte NoTrace/NoCpu
classifier and all eight 2782-byte NoTrace forwarding bodies, with unchanged
byte/ordered-call hashes. Build label: `v0.18.1-39-g67e26af7-dirty`.

The draw-hook CPU log now states its actual scope in both report branches:
subtract the first forwarding interval, including indexed-instanced
weapon-motion reissue, include later EDVR reissues, and exclude any claim of
total EDVR CPU. The numerical fields, prefix, sampling and calculations are
unchanged; the production source gate checks both truthful suffixes.

The isolated `codex/plugin-performance-control-20261003` checkout starts
from the same main revision. It carries only the five timed-draw denominator
regions and helper from `03049f8d`, the same corrected scope text, and the
identical mapped-DLSS reporter plus cold creation insertion. A
dependency-free control rig tests the same meaningful denominator cases and
source wiring, with immediate no-read/no-write dry-run. Actual helper/test
bytes and call-site context match; all three ignored dependency trees match
path/size/SHA with no missing, extra or nested files. It is undergoing its
own full validation. Control acceptance and clean promotion are still
required before installation.

Steam remains on 4350a281. Its current `advanced.draw_replay = on` setting
allocates opt-in diagnostic pools, so Sean has been asked to change only
that value to `off` before the matched production timing runs. No new flight
is requested until a concrete promoted control is verified. Remaining Phase
1 coverage/performance gates and the Phase 2 hold are unchanged.

### Validated matched control ready in Steam, 2026-10-03

Control `373198c16538609638a8c1df622800e288a448e7` is pushed on
`codex/plugin-performance-control-20261003`. Its final full validation
passed all 124 jobs: 119 pooled jobs in 159.3 seconds and five quiet jobs in
23.4 seconds. Production profiles, Python self-tests, native rigs, the
234-key contract and self-contained installer checks pass. FocusWatch
reports no shown window, console or foreground move to the build tree in 183
seconds. Receipt input SHA256:
`556b1a90bf51565b9ee32a678aaf159e4513c1b0128e68b2c5604ef9fc0a4939`. The full
build was stamped `v0.18.1-18-g83938927-dirty`; the clean receipt-verified
promotion is `v0.18.1-19-g373198c1`.

The initial nested-control run failed 13 native bootstrap assertions because
package discovery uses exactly two parents above the loaded module directory
for `d3d11.dll`. From `build/perf-control-main839/build`, that found the
outer feature build's valid proxy, triggering
`module_configuration,source=packaged-default` before the fixture's explicit
configuration. Changing CWD would not change the module-derived path. The
intact control was moved to ignored `build/control-scratch/main839`, whose
derived graphics path `build/control-scratch/d3d11.dll` is absent. No
production or test source was changed; the next complete build passed.

Ruled out: broken control runtime source, because the exact discovered outer
proxy caused early packaged-default configuration and source-identical
validation passed after path isolation.

Candidate `ecdda1d9492256d527b34c7e4d9b529392b396c0` passed clean promotion
as `v0.18.1-48-gecdda1d9`, using the matching 142-job full receipt. A prior
promotion attempt was refused by another build's global lock even though its
shell exit code was zero. It was rejected from the actual log, and the retry
requires the explicit DLL-only success marker. No blocked build is treated
as a successful promotion.

The sanctioned Steam installation of control `373198c1` passed dry-run,
transactional installation and `--verify-only`. Existing DLSS was preserved;
both builds will observe its mapped version in the fresh log. Sean changed
only the replay setting to off; the installer preserved the resulting
personal INI SHA256
`EC41FF141C496A4783B38EA46AF79D5E78E817A8A9F241A0193040CAA25B1015`. Native
receipt backup: `edvr_native_receipt.json.pre-373198c1-20261003-111452.bak`.
The running game detected during installation was Epic; the target-specific
guard confirmed Steam was stopped and Epic was not modified.

The next scoped matched baseline uses the same Pimax OpenXR environment at
90 Hz and 4032x3898 per eye: stationary on-foot hangar AA off, hangar DLSS
on, carrier AA off, carrier DLSS on, then carrier DLSS+night vision, two
minutes per hold after menus close and the mode settles. No NumLock draw
dump. The prior verified flight's 1800-frame reports span about 20-24
seconds, so each hold should contain several completed windows; transition
windows are excluded from comparison. The first fresh-log check is
build373198c1, then actual loaded DLSS, render dimensions, workload/sample
counts and direct GPU sections. A candidate comparison and any narrowly
needed repeat remain pending. Per-timed-draw CPU is the declared
hook-minus-first-forwarding interval; it does not establish total EDVR CPU
or full GPU/module coverage. Phase 1 acceptance, Phase 2 and shipping stay
open.

### Measured Steam control, 2026-10-06

The sanctioned log reader verifies graphics log `edvr_gfx_20261006_174238.log`
as control `373198c1`, `v0.18.1-19-g373198c1`, PE `6AC133AA`. The paired
runtime log is `edvr_openxr_20261006_174240_034_14868.log`. The mapped DLSS
file reports version `310.9.1.0`. Observed environment: RTX 5090, Pimax OpenXR,
Pimax Crystal Super, 90 Hz, 4032x3898 output per eye, 2016x1949 input per eye,
HMD quality 0.50, DLSS performance preset K, separate-device graphics
ownership. Replay is off. Sean confirms carrier first, with NV enabled for the
last two minutes of its DLSS hold, then on-foot hangar. The log has no
NV-toggle marker, so it cannot split the carrier DLSS hold into independent
NV-off/on bins.

The conservative settled draw-hook windows give:

| Scene and mode | End frames | Timed draws | Weighted us/timed draw | Mean window ms/sampled frame |
|---|---|---:|---:|---:|
| Carrier, AA off | 16200-23400, five windows | 29588 | 0.1154 | 0.3884 |
| Carrier, DLSS K, includes reported NV | 28800-43200, nine windows | 53232 | 0.3225 | 1.0862 |
| Hangar, AA off | 54000-61200, five windows | 24646 | 0.0886 | 0.2482 |
| Hangar, DLSS K | 66600-73800, five windows | 24592 | 0.1650 | 0.4614 |

Per-draw means weight each logged mean by its timed-draw denominator. The
interval subtracts the first forwarding call, including an indexed instanced
weapon-motion reissue, but includes later EDVR reissues. It is not total EDVR
CPU time. Startup, mode boundaries and the final transition are excluded. The
hangar DLSS sample-frame means decline from 0.547 to 0.406 ms; a single run
does not establish repeatability or improvement.

Native benchmark medians of completed-window p50 values are CPU/GPU 3.854/5.811
ms for carrier off, 5.085/10.792 for carrier DLSS, 1.901/5.386 for hangar off
and 2.2915/8.1735 for hangar DLSS. The selected completed windows are 9-11,
13-19, 25-27 and 29 plus 36 respectively. Startup carrier windows 6 and 7 and
all scope-changed windows are omitted. Only two hangar DLSS benchmark windows
completed; these are medians of per-window percentiles, not percentiles over
pooled samples. CPU p50 ranges are 3.721-4.027, 4.667-5.696, 1.867-2.058 and
2.286-2.297 ms; GPU ranges are 5.632-5.927, 10.631-10.837, 5.336-5.632 and
8.161-8.186 ms. These native render intervals and partial GPU sections do not
establish whole-module cost coverage. Runtime reports no temporal failures.

Post-flight native verification initially failed because the expected
configuration spelled `C:\Steam` while the installed paths spell `c:\steam`.
Repeating `install_edvr.py --verify-only` with that recorded target spelling
passes all payload/profile checks. `separate_device=1` is already the control's
standard configuration, not a changed setting. No live configuration was
edited. Current personal INI SHA256 is
`750BB1D392EA59270B12CB54051F7E476D28433351B9DA238BF0BFA4B520B91C`.

The measured control is based on main `83938927`. Main was fetched and frozen
at `465e3eddf4769ae5833ae726658170b380394d45` for integration after this
measurement. Its shared animated weapon-history change can affect VR motion
work; this flight logged five eligible weapon-motion calls per frame while DLSS
was active, and `fix.weapon_stability` is enabled. A candidate that includes it
cannot attribute every difference against this older control to plugin
dispatch. Preserve this baseline and validate an updated symmetric control
before the next attributable paired comparison. Phase 1 performance acceptance
and whole-ladder gates stay open.

### Main 465 integration after measurement, 2026-10-06

Main `465e3eddf4769ae5833ae726658170b380394d45` is merged into the feature
after the October 6 baseline was measured and independently checked. Seven flat
draw-wrapper conflicts retain the feature's trace/bypass structure and main's
`needsActualDraw()` brackets, including indirect `args/off`. Independent
three-way review confirms all main changes in `vscreen.cpp` and `build.bat`
remain. The new flat SDK proxy, integration rig, three self-test exports and 64
shader variants are preserved. Main's new `advanced.temporal_aa_jitter_phases`
key belongs to `temporal-aa`; no key is removed or renamed.

The first full build compiled the production DLL but caught a missing
`cockpitVisualsPluginOps` symbol when relinking the new offline bench.
`build_flat_sdk_bench_proxy.py` now links the production cockpit archive
exactly once when the production object set includes `plugin_registry.obj`. It
rejects a missing or out-of-build archive and ignores a stale optional archive
for a main-only control. Self-tests pin those cases and the no-write dry run.
The complete rerun passes the bench's real link and integration rig.

The unchanged runner's timeout-cleanup self-test repeatedly failed in the
restricted process context, leaving test grandchildren alive. It passes with
normal Windows process permissions, as do both complete builds in that context.
No runner assertion was weakened; no Windows error code was observed. Ruled
out: a main-source regression in timeout cleanup, because runner bytes are
identical and the same self-test and full gates pass with the required process
permissions.

Feature validation passes all 143 jobs: 136 pooled in 167.8 seconds, seven
quiet in 48.3 seconds. FocusWatch reports no window, console or foreground move
to the build tree in 216 seconds. Production profiles, Python tools, native
rigs and actual self-contained installer resources pass. Full-build receipt
input SHA256:
`01d9f72a3d54e2dad859b01874d4ad80f132ce50e63215e8187b5985989b934d`. Fresh MSVC
NoTrace/NoCpu comparison matches the saved classifier exactly: 7057 bytes, 1636
records, 94 calls, 288-byte stack and SHA256
`2647f0304a6ed9d294791c5d09d650376843065afdf25660406aaab53fd3ca40`. All eight
NoTrace forwarding bodies also match: 2782 bytes, 756 records, 71 calls,
176-byte stack and SHA256 prefix `6ea003ac45a3e806`. This proves the scoped
instrumentation erasure, not whole-ladder equivalence.

Updated control `7bbe7d907bcf0b5f2e969e1d3373e9c37ba5f5ed` includes the same
main pin and retains only the symmetric denominator and mapped-DLSS
diagnostics. It passes all 125 jobs: 120 pooled in 157.6 seconds and five quiet
in 23.1 seconds; FocusWatch reports no shown window, console or foreground move
in 181 seconds. Receipt input SHA256:
`d62d3bdbeb2ac5f44041ea909100517bda45e010ea11018e96c761dd065c5c53`. The
draw-window header, DLSS helper and DLSS test are byte-identical across both
trees. The feature's per-frame plugin-cost collector remains part of its
production overhead and lies outside the declared draw-hook interval. These
partial timings do not establish total EDVR CPU coverage.

Both clean commits require receipt-verified promotion before installation.
Steam still holds measured control `373198c1` while that completes. Preserve
the personal INI, loaded DLSS, separate-device mode and render dimensions for
the next matched control/candidate comparison. The original baseline is
retained; Phase 1 performance acceptance, Phase 2 and shipping stay open.

### Updated matched builds ready in Steam, 2026-10-06

Clean receipt-verified promotions pass for control `7bbe7d90`
(`v0.18.2-60-g7bbe7d90`) and candidate `6c63f6aa` (`v0.18.2-90-g6c63f6aa`).
Both source commits are pushed on their separate branches. The subsequent main
commit `501e2b15` changes only the terrain investigation document; it is also
merged into the feature. Rendering source and receipt inputs remain those
validated against main `465e3edd`. The candidate DLLs identify source commit
`6c63f6aa`, not the later documentation merge; use that explicit build pin when
reading its future flight.

The sanctioned control Steam install passes dry run, transactional install and
`--verify-only`. Existing DLSS is preserved and the personal INI SHA256 remains
`750BB1D392EA59270B12CB54051F7E476D28433351B9DA238BF0BFA4B520B91C`. The
configuration retains `separate_device=1`. Native receipt backup:
`edvr_native_receipt.json.pre-7bbe7d90-20261006-183751.bak`.

Next flight: verified control `7bbe7d90`, Pimax OpenXR, 90 Hz, 4032x3898 output
per eye, unchanged HMD quality and DLSS. Stationary carrier AA off, DLSS with
NV off, then DLSS with NV on; on-foot hangar AA off then DLSS. Each hold is two
minutes after menus close. Replay stays off; no NumLock dump. Exit Elite
afterward. Read the fresh log with `--expect-build 7bbe7d90` before evaluating
timings or visuals. Then install and fly the candidate under the same
conditions. Native builds are complete for the pair. The October 6 control
`373198c1` measurement remains retained; no plugin performance improvement or
Phase 1 acceptance is claimed yet.

### Why the holstered hangar still exercises history, 2026-10-07

Sean points out that neither the carrier cockpit nor the on-foot hangar lets
him draw a weapon. The earlier rationale cited eligible calls without tracing
the completed command intervals. That evidence alone does not identify a
rendered weapon, the mesh that matched, or pixels receiving valid vectors.

Re-read the exact October 6 graphics log through `edvr_log.py --file` with
`--expect-build 373198c1`; the build matches. During the hangar interval, scope
2 at 17:54:34.279 records 1800 source frames and 9000 eligible calls. Its
identity, post-VS capture and raster reports each contain 141 selected, 141
submitted and 141 ready samples, with no invalid or pending samples. The other
six reported scopes likewise contain completed capture and raster samples.
These are sampled GPU command intervals, not a count of visible weapons or an
estimate of all unsampled work.

The indexed-instanced hook calls `weaponMotionDraw` for matching vertex-shader
hashes after the original draw, on the owner context and the on-foot source
path. Admission checks shader family, current source, geometry, viewport and
depth/stencil state; it does not query whether the player equipped a weapon.
The new shared `AnimatedVertexHistory` prepares the capture and supplies its
identity/history work before position capture and motion-map rasterization. The
reported samples therefore establish executed work on this path in the
holstered hangar. Which mesh triggered it remains unproven.

Ruled out: a holstered weapon makes the weapon-history path inactive in this
hangar baseline, because capture and raster intervals completed. Do not infer
weapon visibility from the feature name or the eligible-call counter.

Read-only Luna source review confirms main's engine-velocity domain/marker
additions are flat-profile gated; the carrier's existing VR engine-velocity
activity is not evidence those additions executed. The carrier comparison is
the cockpit/plugin workload for the control and candidate with common main
source, rather than a weapon-quality test. The fresh hangar control is needed
to separate the shared history change from plugin overhead. Keep the previous
baseline as evidence, preserve the holstered scenes, and make no weapon-visual
or plugin-performance acceptance claim from these counters.

### Measured common-main control and installed candidate, 2026-10-07

Sean confirms carrier AA off, DLSS with NV off, DLSS with NV on, then on-foot
hangar AA off and DLSS, two minutes per hold, with no visual change. The exact
graphics log `edvr_gfx_20261007_051735.log` passes the sanctioned reader's
`--expect-build 7bbe7d90`: `v0.18.2-60-g7bbe7d90`, PE `6AC592E4`. Its paired
runtime `edvr_openxr_20261007_051737_047_55460.log` independently matches that
build. Pimax OpenXR / Crystal Super remains at 90 Hz, 4032x3898 output and
2016x1949 DLSS input per eye, preset K, separate-device mode 1. The mapped DLSS
library is `310.9.1.0`. Shutdown returns without an exception; lifecycle stages
complete, frame-cycle accounting is intact, and direct GPU timing reports no
failed spans. No dump or replay sidecar was captured.

The new on-foot source is 3872x2178; the live width setting is `auto`. The
October 6 control's source was 5120x2880, so do not mix that older measurement
into this matched pair. Native benchmark rows contain the render dimensions but
do not independently record HMD quality; its unchanged setting relies on Sean's
protocol confirmation.

Night vision engagement is positively logged at 05:25:06.132. There is no NV
off marker. The UI maps ON messages at 05:22:58.443 and 05:30:23.737 are AA
activation snapshots of the journal, not scene transition timestamps.
`ui_layer.cpp` emits them when the key and screen-motion service become active.
Ruled out: the first on-foot ON message means the earlier AA-off hold was still
aboard, because AA off suppresses this instrument and Sean confirms the hangar
hold. Scene labels below use that operator evidence. Initial Luna selection
incorrectly omitted both operator-labelled AA-off phases; the next-tier review
repaired the selection and an invalid trailing JSON escape.

All 40 native benchmark rows and 44 draw-hook rows were independently checked
against sanctioned reader output. Conservative settled bins are:

| Phase | Draw-window end frames | Timed draws | Weighted us/timed draw | Weighted ms/sampled frame | Native windows | Native CPU/GPU median p50 ms |
|---|---|---:|---:|---:|---|---:|
| Carrier, AA off | 18000-25200 | 28515 | 0.117983 | 0.382902 | 14-16 | 3.482 / 5.674 |
| Carrier, DLSS, NV off | 30600-37800 | 30836 | 0.320056 | 1.121945 | 20-22 | 5.384 / 10.801 |
| Carrier, DLSS, NV on | 41400-48600 | 26156 | 0.363235 | 1.079597 | 24-26 | 5.028 / 10.818 |
| Hangar, AA off | 59400-66600 | 26379 | 0.097019 | 0.291171 | 33-35 | 2.441 / 4.517 |
| Hangar, DLSS | 70200-77400 | 26231 | 0.169289 | 0.504980 | 37-39 | 2.788 / 6.486 |

Each draw bin has five completed windows. Per-draw means weight logged means by
their timed-draw denominators; frame means weight by sampled-frame counts. The
draw interval has the same first-forwarding subtraction limitation as October 6
and is not total EDVR CPU. Native values are medians of the three completed
windows' p50 values, not pooled frame percentiles. Mode changes, NV-on
crossing, travel/source changes and final menu windows are excluded. The
ignored reproducible report is
`build/control_plugin_measurement_20261007.json`; scene snapshots and positive
NV engagement are kept separately from operator-labelled phases.

Control payload verification passes before installing the candidate. The
candidate `6c63f6aa` had already passed the complete 143-job build, fresh
codegen comparisons and clean receipt-guarded promotion as
`v0.18.2-90-g6c63f6aa`. The sanctioned install now passes dry run,
transactional install and `--verify-only`. It preserves the personal INI SHA256
`88284200D1BB3FF4681CDFE67D0C06614EABF1799F7A047CE7494B18ADD3D839` and existing
DLSS SHA256 `3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983`.
The live candidate setting is explicitly `draw_replay = off`; the control does
not implement that feature and ignored the key. Native receipt backup:
`edvr_native_receipt.json.pre-6c63f6aa-20261007-053456.bak`.

Next flight is the candidate under the same five holds, with menus closed,
stationary matching views, replay off and no NumLock dump. Native builds stay
idle. Pin the fresh logs to `6c63f6aa`, check actual source dimensions and
workload counts, then compare with this control. No plugin gain, repeatability,
GPU non-regression, Phase 1 completion, Phase 2 or shipping approval is claimed
from a control-only measurement. Keep both source revisions frozen until the
pair is measured; the feature stays separate from main.

Read-only Luna preparation identifies the next bounded coverage slice: the warm
PanelDistance path's saved-original Map, Unmap, override constant-buffer bind
and restore bind bypass shared hook counters. Attribute those actual calls to
OnFootPanel with one claim-scoped API sample latch carried through restoration;
preserve Map failure and unsampled behavior. Cold allocation is outside that
slice. This is a review proposal, not an implemented or validated change, and
waits until the measured pair is adjudicated.

### Common-main candidate comparison, 2026-10-07

Control `7bbe7d90` and candidate `6c63f6aa` are now measured. The graphics logs
are `edvr_gfx_20261007_051735.log` and `edvr_gfx_20261007_055115.log`,
respectively; the sanctioned reader verified each build before analysis. Their
runtime logs are `edvr_openxr_20261007_051737_047_55460.log` and
`edvr_openxr_20261007_055117_046_44884.log`. Both use RTX 5090, Pimax OpenXR
Crystal Super at 90 Hz, 2016x1949 input and 4032x3898 output per eye, mapped
DLSS 310.9.1.0 preset K, native `separate_device=1`, and replay off. Sean
confirms carrier first, then on-foot hangar, following the prescribed holds; he
reports no visual changes. Both exit cleanly with no rejected, invalid or
cancelled handoffs. Each runtime log has two cycles exceeding 250 ms; the
candidate also has two transient head-locate failures (-30). These observations
do not establish a cause or a stall-free run.

The independent audit verified all 37 candidate native benchmark rows and 48
draw-hook rows. Native window IDs selected by phase are 12-14 / 17-19 / 21-23 /
30-32 / 34-36. Selected draw-window end frames are 27000-34200 / 37800-45000 /
48600-55800 / 64800-73800 / 77400-84600, every 1800 frames. The hangar AA-off
selection has six draw windows; all other selections have five. The control
selections remain those recorded above. AA activation snapshots do not date
scene transitions; operator-confirmed sequence supplies scene labels. Positive
NV markers are 05:25:06.132 for control and 05:59:19.742 for candidate.

Sampled draw-hook own time, after the existing real-draw subtraction:

| Phase | Control -> candidate us/timed draw | Change | Control -> candidate ms/sampled frame |
|---|---:|---:|---:|
| Carrier AA off | 0.11798 -> 0.11898 | +0.85% | 0.38290 -> 0.27720 |
| Carrier DLSS, NV off | 0.32006 -> 0.29905 | -6.56% | 1.12194 -> 1.10571 |
| Carrier DLSS, NV on | 0.36324 -> 0.30888 | -14.96% | 1.07960 -> 1.03894 |
| Hangar AA off | 0.09702 -> 0.09871 | +1.74% | 0.29117 -> 0.25491 |
| Hangar DLSS | 0.16929 -> 0.17841 | +5.39% | 0.50498 -> 0.48757 |

Only NV-on has the entire candidate per-draw window range (0.294-0.328 us)
below the control range (0.331-0.398 us); every other aggregate lies within the
control's observed range. This is a preliminary favorable NV-on signal, not a
repeatability or attribution result. Timed draws per sampled frame change by
-28.30% / +5.53% / +13.07% / -13.82% / -8.36%, respectively. Normalization
accounts for unequal window counts but does not equate shader or action mixes.
Lower per-frame means therefore cannot alone prove a plugin CPU improvement.
Native CPU/GPU benchmark medians also include game/runtime work and do not
replace complete EDVR cost coverage.

The independently corrected direct GPU comparison uses all 32 completed
30-second census intervals wholly contained within the five phase boundaries,
not intervals clipped to native benchmark endpoints. The selected rows contain
147,398 timed spans and zero failed spans. Frame-weighted EDVR GPU subtotals:

| Phase | Control/candidate windows | Control -> candidate ms/frame | Change |
|---|---:|---:|---:|
| Carrier AA off | 3/3 | 0.011000 -> 0.011334 | Below display precision |
| Carrier DLSS, NV off | 4/3 | 5.863693 -> 6.034487 | +2.91% |
| Carrier DLSS, NV on | 3/3 | 5.999020 -> 5.890454 | -1.81% |
| Hangar AA off | 3/4 | 0.012668 -> 0.011000 | -13.17% |
| Hangar DLSS | 3/3 | 1.763675 -> 1.796999 | +1.89% |

Carrier NV-off is a preliminary adverse result: candidate median 6.017 ms
exceeds control maximum 5.991 ms, though the ranges overlap narrowly. Dropping
the control census ending only 54 ms before the positive NV marker still leaves
+2.20% and the candidate median above the control maximum. NV-on is
preliminarily favorable: all candidate subtotals are below all selected control
subtotals. Hangar ranges overlap; its DLSS point estimate is adverse. The
AA-off absolute differences are very small. These are direct EDVR subtotals
with incomplete module coverage, not whole-game GPU totals; pooling NV-on and
NV-off would hide the adverse cell.

The automatic on-foot source is 3872x2178 in control and 3952x2223 in
candidate, 4.175% more pixels, despite unchanged output eye sizes and preserved
INI. That difference cannot explain the carrier result. The hangar DLSS weapon
scope records 78.24 versus 62.78 wrapper entries per frame, not admitted
captures: its census begins before early returns. Both detailed reports have
9000 eligible calls over 1800 frames (five per frame), 141 selected/ready/
submitted identity/capture/raster calls, and no skips, failures or pending
captures. A holstered weapon does not bypass this shader/depth classifier; the
counters do not identify a visible weapon or a particular mesh.

Ruled out: equal output eye sizes establish equal workload, because source
pixel count and timed-draw density differ. Ruled out: lower native GPU medians
establish EDVR GPU non-regression, because the direct NV-off subtotal
increases. The Phase 1 performance gate remains open. No rendering fix is
established by this pair; no additional flight is requested yet. Keep
`7bbe7d90` and `6c63f6aa` frozen while reviewing the adverse carrier scopes and
their work counts; distinguish changed workload, timing variation and
additional EDVR work before selecting a rendering fix. The 5.53% higher NV-off
timed-draw density is a discriminator to inspect, not proof of the cause. The
warm PanelDistance API policy remains an unimplemented coverage proposal.

Sean notes that carrier ship types and quantities vary between runs. Variable
scene workload is a plausible explanation for the higher NV-off cost; the
increased sampled draw density is consistent with that hypothesis. Inspect the
measured GPU scope costs and their work counts before attributing the change to
the plugin layer. Draw counts alone cannot establish causation.

The bounded scope audit confirms NV-off door cost rises 0.128420 ms/frame and
in-frame cost rises 0.042123 ms/frame; their sum differs from the 0.170793
subtotal delta by 0.000250 due to displayed-row quantization. Within the door,
upscaler time rises 0.118731 ms/frame at two entries per frame in both builds.
The UI reissue scope rises 0.062104 ms/frame with 81.58 -> 100.26 entries per
frame (+22.9%). Those entries count helper attempts after the blocked-path
check, before multiply/write-back begin helpers that can decline
(`vscreen.cpp:4915-4950`); they are neither successful redraws nor ship counts.
The census estimates corrected mean sampled time per entry times all entries
per frame (`gpu_census.cpp:168-180`), with capped round-robin sampling. These
component figures sit within the subtotals; do not add them to the subtotal
again. NV-on UI attempt counts also rise (72.77 -> 78.10/frame), while their
cost falls 0.019659 ms/frame. Increased counts alone therefore do not explain
the duration change. Unchanged upscaler counts do not rule out scene content or
GPU/queue timing changing per-call duration. Variable ships remain a plausible,
unproven contributor; this pair does not establish an architecture regression.
Existing engine-motion pool/joined/moving/history summaries and native
ROI/submit-window fields can provide workload context before another flight.
The performance gate stays open.

Reproducible ignored reports:
`build/candidate_plugin_measurement_20261007.json`,
`build/candidate_plugin_independent_comparison_20261007.json`, and
`build/plugin_direct_gpu_comparison_20261007.json`. The latter includes the
corrected phase-contained row selection and near-NV-boundary sensitivity. The
feature stays on `codex/plugin-architecture`; no merge to main, Phase 1
acceptance, Phase 2 or shipping approval is claimed.

### Main refresh and warm PanelDistance API cost, 2026-10-07

Main `fcdc3c05` is integrated into the feature tree after Sean lifted the merge
hold. Its planet-patch motion and supercruise HDR orbit/bar/dust changes are
preserved alongside the existing trace actions and forwarding. The conflicts
retained both sets of build rigs. Night vision still shuts down through the
cockpit plugin registry; scheduler/static configure and reload remain owned by
vScreen, avoiding duplicated lifecycle calls from the older main layout.

The bounded API slice records the four saved-original calls in the warm
PanelDistance transaction: Map and Unmap at OnFootPanel sites 122/123
(Transfer), override and restore VS constant-buffer binds at 124/125 (State).
The fixed 128-site limit and 1328-byte snapshot ABI are unchanged. Selection is
independent of CPU sampling and replay. A thread-local hint avoids a context
query on unselected frames; positive hints still pass the atomic context and
owner-token checks before the collector reads owner-only sample state. The
constant-initialized hint has the specific MSVC `no_tls_guard` attribute, and
only the small outer dispatcher is forced inline.

The actual-production test seam runs the prequalified warm Site 66 claim,
forwarder and restore with injected saved-original callbacks. It checks all
four draw families' typed arguments, single issuance, Map/Unmap/bind/draw/
restore order, scaled/copied constants and final binding. Failed Map and null
mapped data decline without later panel calls. NoApi preserves the transaction
with zero counters/bits; API sampling works with NoCpu and CPU trace
suppression. Collector tests cover owner registration, lifecycle transitions,
masks and a stale positive hint after quiescent owner transfer. This is not the
complete eligibility ladder. The first native failure incorrectly expected a
foreign exit from this prequalified seam: it now checks the same transaction
with NoApi; separate production Site 2 tests still require foreign-context
exit.

The incoming orbit and bars scanners initially rejected the trace action
between `uiLayerEnd` and second issues. Production ordering was correct. Both
now pin exact traced or plain second-issue arguments independently and retain
the full binding/issue/restore order. Their late-restore controls now move the
actual restore, and missing-second-issue controls are checked. All five
incoming rendering rigs pass together on captured fixtures and WARP (27.2 s,
zero FocusWatch events); these corrections change no rendering code.

Fresh reference/current listings use the same merged source revision, flags and
original source path. The saved pre-API reference hash is `d4e4d28c`; current
production source hash is `345ea597`. All three NoApi classifier variants
preserve bytes, records, stack and ordered calls. The NoTrace/NoCpu variant
remains 7057 bytes, 1636 records, 94 calls and a 288-byte stack (SHA-256
`2647f030...`). All sixteen NoTrace forwarding variants and all four
draw-callback families preserve exact bodies, stack and call order. A changed
local symbol is accepted only after its body and raw callees match. The
forwarders retain all three callback call positions (15/39/70).

The whole selector is not zero-cost. Each false-hint check has three
instructions/11 bytes and jumps over the context verifier. There is no new hint
TLS initialization guard and no executed outer trampoline; the existing
DrawClock TLS guard is unchanged. Instanced NoApi dispatch still marshals the
empty API argument to an outlined work lambda: 939 -> 988 bytes and stack 288
-> 304. IndexedInstanced dispatch adds argument setup only in its trace branch
(1611 -> 1621 bytes, stack 320 unchanged). Whole-hook static size deltas for
D/I/N/X are +74/+77/+102/+87 bytes, including both branches; X hook stack is
336 -> 344. These are codegen measurements, not measured frame-time costs.
Removing the remaining empty-policy argument cost needs another bounded codegen
review before any zero-cost claim.

The final normal full build passes all 148 jobs: 141 pooled in 166.2 s and
seven quiet in 49.4 s, with zero FocusWatch events over 216 s. Config contract
checks all 236 keys, actual installer resources match the release files, and
the fresh full-pass receipt fingerprints source/dependencies as `5d8aa96c`. The
world-route mutation's old template anchor also needed the third API policy
parameter; all 96 mutation self-tests over four rigs now pass, and the control
still moves cleanup before verdict selection and the original issue.


The earlier `7bbe7d90`/`6c63f6aa` source and flight pair stays frozen. No Steam
install, personal INI edit or flight occurred for this refresh. The new main
rendering and sampler changes are not evidence that the earlier carrier GPU
increase is resolved. Remaining cost coverage, whole-ladder/action parity and
CPU/GPU performance acceptance stay open. Next flight: none until the offline
gates produce a discriminating comparison; Phase 2 and shipping remain held.

### By-value API policy and full classifier exit fixtures, 2026-10-07

The selected work lambda receives the empty API policy by value. The outer
chooser still constructs one capture object and independently selects API
sampling with the existing hint/context check. The selected helper constructs
the tag with `ApiPolicy{}`; a compile-time empty-type invariant prevents member
state from being added silently. Classification, forwarding, restoration and
CPU/replay suppression retain their existing ordering.

Three bounded codegen trials were reviewed against the frozen merged-main
reference and parent `4d6b9c58`. Ruled out: the typed work factory as the
chosen implementation, because it duplicated capture blocks in the two branches
and grew D/I/N/X hooks by 40/54/71/86 bytes. Each draw still executed one
capture block; the growth was static, not extra live stores. It removed the
empty tag marshalling, but the smaller by-value change preserves the parent
hook sizes. Ruled out: default-initializing the by-value tag, because MSVC
emitted a load of its padding byte and an outgoing byte store (N 989 bytes, X
1622), worse than value initialization. The absence of a warning did not
establish erasure.

The chosen value-initialized variant keeps the parent hook bodies' sizes,
stacks and live capture materialization: D/I/N/X are 1166/1207/1319/1469 bytes
with 5/6/7/8 capture stores. Instanced NoApi dispatch shrinks 988 -> 971 bytes,
and IndexedInstanced 1621 -> 1616. D/I dispatch still matches the pre-API
reference. N retains a 304-byte stack versus the pre-API 288, and writes one
unused byte in each selected work branch; X retains a trace-only tag store and
a 320-byte stack. The false-hint check remains three instructions/11 local
bytes and skips the context verifier. No new hint TLS guard or executed outer
trampoline appears. This reduces setup; it does not erase all selector cost.

Strict listing comparisons retain the 7057-byte, 1636-record, 94-call,
288-byte-stack NoTrace/NoCpu classifier fingerprint (`2647f030...`). All three
NoApi classifier variants and all six parent/current policy variants match; all
sixteen NoTrace forwarders and four typed original callback families preserve
bodies, stack and ordered calls. Renamed local targets require exact callee
bodies and raw ordered targets. These proofs are instrumentation and forwarding
gates, not whole-ladder or frame-time acceptance.

The test-only WARP seam now also invokes the full Common+Eye classifier before
the production forwarder. Real panel SRV, eye RTV and matching CB resources
seed the binding shadows. An injected failed Map reaches `PanelTailNone` after
39 literal ordered site/outcome records; disabling distance exits at
`EyeNoDistanceNone` after 36. Both paths preserve None and issue one typed
original callback for D/I/N/X in Trace and NoTrace. The enabled failure path
records Map then Draw, with no Unmap or override/restore bind; the disabled
path records Draw only. Trace checks exact DrawBegin/OriginalDraw/DrawEnd
records; NoTrace emits no site observations. Effects here are the injected
saved-call recorder, not GPU binding equality. Successful Panel selection
through the complete classifier and other claim paths remain unproved.

Review repaired repeated-call eye-cache invalidation and preserved registry
filters by exchanging only the cached legacy interest mask. The trace facade
borrows the live policy's state and forwards its complete interface; an
explicit constructor avoids MSVC's aggregate reference-initializer failure.
Fixture arguments follow the real hooks, including zero unused fields for D/I.
The original action assertions were retained. Focused collector and world-route
gates pass (283 world checks and 96 mutation anchors); the WARP gate passes
after these corrections.

The normal full build passes all 148 jobs: 141 pooled in 178.9 s and seven
quiet in 50.7 s. FocusWatch reports no visible window, console opening or
foreground movement into the build tree. All 236 config keys agree; actual
installer resources match the release files. The fresh full-pass receipt
(`b507662b`, 2026-10-07 15:45 UTC) verifies the same source/dependencies.

The earlier control/candidate flight pair stays frozen. No Steam install,
personal config edit or flight occurred for this slice. Remaining API coverage,
CPU attribution, whole-ladder/actions and CPU/GPU acceptance stay open. The
EngineVelocity API inventory belongs to TemporalAa, but a complete saved-call
slice needs a deliberate site allocation beyond the two remaining IDs in the
128-site mask. Next flight: none until the remaining offline gates produce a
discriminating comparison. Phase 2 and shipping remain held.

### API capacity and draw transaction accounting, 2026-10-07

The API-only collector gains a versioned 256-site report. V1 keeps its original
symbols, 1328-byte window, version 1 and two-word masks; aggregate API counts
include high IDs while its site mask exposes only IDs 0-127. V2 uses a
1488-byte window, version 2 and four API words. CPU IDs stay capped at 127. The
focused collector rig passes C layout/signature checks, V1 output canaries,
high-site aggregate counts, V2 bit boundaries 127/128/191/192/255, rejection of
API 256 and CPU 128, sampled-frame closure and zero other owners. Production
reads V2 only when the report is ready and labels partial coverage.

The successful PanelDistance full classifier fixture checks the literal 38-site
Trace prefix and claim, exact Map/Unmap/override/draw/restore order, scaled
constant bytes, callback arguments and restored CB0 for D/I/N/X. NoTrace
compares the decision and zero ordered observations, because its siteResult
payload is intentionally absent. The prior 39-site failed-Map and 36-site
distance-off controls remain. This is recorded-effect evidence, not an
independent GPU binding oracle or complete ladder parity.

EngineVelocity sites 126-135 belong to TemporalAa and cover target/blend
transactions. Ruled out: treating eager/declined flat draw restoration as
lifecycle-only, because WARP recorded target/blend restore calls with no
matching sampled notes. The higher-tier repair retains strict note-to-actual
call assertions and adds eager, declined, memo, injected MRT6 rollback and
VR/NoApi cases. The repaired focused rig passes 15,758 checks (32.1 s). Legacy
AfterFlatDraw and frame/lifecycle restoration remain deferred.

The initial assembly comparison caught new helper calls in the unsampled
velocity path. Narrow private inline routes retain the original cold-helper
structure. The final original-path comparison against `9e79763e` passes all six
classifier variants, the unsampled velocity entries and slow path, the original
weapon body and recursively checked helpers, and 16 forwarders in four callback
families. The unsampled classifier remains 7057 bytes/94 calls; the slow path
remains 5115 bytes/704-byte stack/95 calls; the weapon body remains 3359
bytes/91 calls. The two sampled X forwarders intentionally call the
same-signature sampled weapon entry. The analyzer records those exact target
changes instead of treating them as unchanged code.

Both supervised compilations used fixed version text, original source paths and
identical flags; all 17 current inputs retained their hashes after the 15-file
parent swap was restored. The failed listings remain archived under
`build/engine-velocity-api-proof`. This proof covers the named leaves and
callback contracts. Boundary selector overhead and whole-path performance
remain outside it; no frame-time acceptance follows from assembly equality.

The parallel weapon-motion slice implements 30 operations, IDs 136-165,
OnFootPanel: seven ReadQuery, 21 State, one Transfer and one Work.
UpdateSubresource is Transfer; two IA apply binds are included; the extra
raster uses `(count,1,0,0,0)`, distinct from the original/capture tuples.
Admission, resource preparation, history capture, clears and GPU timers are
outside this bracket. Linked class-instance lifetime needs a real nonzero
fixture and remains explicitly unproved. The repaired WARP fixture passes
216,262 checks: exact note-to-call order, temporary raster bindings, restored
host state and bytes, shifted nonzero original/capture tuples, zero notes in
NoApi and rejected draws, and the existing motion-output oracle. Ruled out:
expecting host state unchanged during SO capture, because capture deliberately
uses POINTLIST, its capture GS and SO target0. The fixture now verifies those
changes and their resource identity while checking the remaining state.

The first full build compiled the production DLLs, then found a missing
generated-header path in a transitive engine-velocity consumer. An inventory
identified three affected rigs: flat temporal, VR world route and VR camera
census. Adding their GEN include paths exposed two stale tooling assumptions:
the VR mutation compiler still omitted GEN, and the flat temporal source pin
counted one velocity query body although legacy and sampled bodies are now
separate. The repair checks each body independently and retains deletion
controls. All three focused rigs pass. The supervised pure mutation compiler
passes its unmodified control and catches all 28 named mutations (19.1 s),
exercising the repaired GEN path. Its self-test also passes 96 controls across
four rigs.

The next full build passed the repaired tooling gates but stopped at the new
PanelDistance fixture: all four Trace variants fail the exact prefix/action
assertion, while the separate API order and draw/restore checks pass. The
immutable parent `9e79763e` has the same replacement markers as production.
Ruled out: four actions for a Panel claim, because every non-None verdict
records ReplaceDraw Begin/Attempted and End/Applied around its original issue.
The literal six-action expectation checks every field, outcome, flag and issue
count without changing the 38-site prefix. The repaired focused rig passes for
D/I/N/X (7.9 s). Separate failure diagnostics now distinguish payload, prefix
and action failures. The failed full-build log is retained as
`build/full-plugin-api-transactions-20261007-panel-failed.log`.

The full absolute-path build passes all 148 jobs, production DLLs and
self-contained installer checks; its rig pool reports no shown windows,
consoles or foreground changes (241 s). The fresh full-pass receipt fingerprint
is `c55f15b0a2f4ae38943ef9b3deb4c07bc52a3ab8d52ad79d96bde230fcf97137`,
independently verified before commit. The unchanged 17-input assembly proof
also passes. Checkpoint `445e0ef6` is committed and pushed. No Steam install,
config edit or flight occurred for this slice. The previous pair stays frozen;
Phase 1 performance, whole-ladder/actions and remaining coverage remain open.
Phase 2 and shipping stay held.

The main refresh now targets `84b6f760`, including `8fa6d443`'s on-foot split,
`8def3d66`'s draw-local history refusal and `4c69779f`'s first-person naming
veto. The sole include conflict retains the required journal header without
restoring the feature's removed scheduler/static headers or duplicate
configuration. Automatic merges retain the query pins and 30-site API ledger
alongside the new covered-draw fixtures. The occurrence-window extension is
requested only by the flat foreground adapter; its legacy default stays four.
Ruled out: rejecting the 64th prior record, because its pre-append index is 63;
only a 65th prior reaches the refusal predicate.

The fresh parent/current assembly comparison passes after compiling both stages
with the merged shared headers (10.9/10.5 s). All 15 temporary source swaps
restore exactly, and the 17 current inputs keep their hashes. This proves the
scoped API erasure against common merged header inputs; it does not claim
equality of the old and new flat history implementations or their performance.
The pre-main proof and checkpoint receipt are archived in the ignored build
directory. The full merged-tree build passes all 149 jobs and self-contained
installer checks; its pool has no shown windows, consoles or foreground changes
(246 s). The engine rig passes 15,758 checks, weapon motion passes 217,415 WARP
checks including the new covered-draw cases, and the on-foot split passes 4,084
checks. The fresh receipt fingerprint is
`2779be20ffaf3c803f4f6f70a18bd9bf2181ab886b6659851de0aefe93f3a7e7`,
independently verified before the merge commit. Steam remains unchanged.

### UI forwarder action parity, 2026-10-07

The next bounded contract exercises the real UI redirect and depth/stencil
writeback through `forwardWithVerdict(kNone)`. The independent expectation has
nine actions: draw begin, UI begin, original issue, UI end, declined multiply
begin (marker 1), writeback begin/issue/end (marker 2), and draw end. It
requires an admitted alpha/premultiplied draw with writable depth/stencil; a
true multiply draw with such writes is refused earlier. Two actual callbacks
retain the same typed tuple: the first targets the UI layer/private depth, the
second targets zero colour views and the original DSV. Host bindings must
restore afterward.

Core-owned UI sites 83-96 cover six reads and eight state calls/restores. Their
sampling is chosen independently inside UI Begin and latched through End; the
surrounding classifier's NoApi type alone does not disable these legacy notes.
Writeback calls and selector queries remain outside this annotated subset.

Ruled out: a partial copy snapshot for repeated UI cases, because the module
owns move-only timers, unique resources, atomics and private fault budgets.
Each case instead starts in a fresh child process; the parent never executes
the UI path. Cases compare pointer-free actions, calls and pixel fingerprints,
including a repeated case. Actual module shutdown and host cleanup run before
case reporting. This avoids altering any parent's UI counters or resources.

A narrow test-macro temporal-input override supplies the sequence, eye, size
and jitter normally provided by a begun native runtime channel. Real family
classification, surface learning, door admission, depth seeding, bindings and
writeback remain under test. Production temporal input remains unchanged.

The first focused native gate compiles/links and passes dry-run, but the new
child fixture fails. Direct child `1 1 0` exits 1 before its final diagnostic
with no output. That early setup failure is indistinguishable in the parent
report from an unexecuted case; the mid-tier repair adds stage diagnostics and
bounded captured failure output before pursuing the specific failed input.
Production behavior and the strong GPU/action expectations stay unchanged.

The diagnostic identifies two missing fixture inputs: trace arming requires an
existing `edvr_gfx_*.log`, and temporal admission requires a 16-byte CB shadow.
The repaired child supplies both and pins the actual all-decline winner,
`kEyeNoDistanceNone`. Ruled out: a mode/size gate in `uiDepthEyeOfTarget`,
because that helper has no such gate. All GPU, action and binding expectations
then pass; the API expectation still misses four cold surface inspections. Each
calls sites 112, 113 and 115, adding twelve reads to the six UI reads and eight
UI State calls. Sampled success requires exactly eighteen reads/eight State
calls and mask `0xb0001fff80000`; the unarmed-door refusal requires twelve
reads/no State calls and mask `0xb000000000000`. Unsampled cases require zero
notes. These totals/site sets do not prove note ordering or whole-route D3D
coverage; writeback and other selector work remain outside the ledger.

Ruled out: a failed child after its successful GPU checks, because its exit is
zero and its complete result ends in CRLF. The parent parser now accepts
exactly one complete LF or CRLF frame, with no extra/truncated data. Child
launch uses hidden creation, valid inherited handles, bounded output and a
120-second timeout. Actual module shutdown precedes success reporting.

The focused WARP gate passes all six fresh-child cases: sampled/unsampled
success with and without Trace, a repeated success, and an unarmed-door
refusal. Success requires two real draws, nine independently expected actions,
every colour/depth pixel, identical surface hashes and complete binding-slot
pointer/generation/hash restoration. Refusal requires one host draw and three
actions. This is a D-only, depth-only D32 fixture; other draw kinds, stencil,
linked class instances and other claimed families remain unproved.

### Substitution-triggered held restoration, 2026-10-07

The bounded velocity slice accounts for an ordinary substitution-triggered
`kOtherDraw` that ends a held producer run. The runtime retains its pending,
owner-thread and exact-context gates. A dedicated boundary rechecks pending
state before selecting sampling once; the original NoApi flush remains
unchanged. Actual blend and target restoration emit State sites 135 then 132
only when their respective generation guards allow the real setter. Domain
entry's separate `kOtherDraw` caller, other causes and frame/lifecycle restores
remain on the original entry and outside this slice.

The focused WARP rig passes 16,158 checks, including real setter/state parity,
generation changes, hint-off, sampler-context refusal, repeat flushes, shutdown
with a stale positive hint and the direct NoApi entry. Mid-tier review found no
production blocker, but four oracle gaps require repair: direct no-pending
boundary coverage, independent RTV/DSV generation mutations, exact API classes,
and source/mutation controls for the real runtime caller. The Emu route alone
does not execute that caller. Foreign-thread/configure/owner transfer remain
inherited collector contracts; new boundary coverage is not claimed for them.
The original flush and recursive NoApi closure gate passes on the previous
merged listing; fresh codegen and the full edited tree remain pending.
Runtime-caller machine cost is outside that three-TU listing scope.

The mid-tier repair adds all four controls, including six in-memory caller
mutants and independent wrong-class/unexpected-site rejection. The focused rig
then passes 16,279 checks. The original NoApi production entry remains
unchanged.

Fresh parent/current listings pass all seventeen scoped assembly gates,
including the original flush and its recursive NoApi restoration closure. All
fifteen temporary swaps restore exactly and seventeen current inputs retain
their hashes. The two planned sampled weapon callee deltas remain explicit;
this is scoped erasure proof, not whole-tree byte identity or timing evidence.

The new public boundary is 308 machine bytes with 67 instruction records and an
80-byte local stack allocation. Its no-pending path has twelve instructions,
forty fragment bytes and zero calls before TLS access. Hint-off has twenty-five
instructions/103 bytes and one original flush call; verifier refusal has
twenty-nine/119 and the verifier plus original flush. Accepted sampling has
fifty-three/252 and one verifier, one mutex lock and one sampled restoration,
followed by a tail unlock. Counts exclude callees and the runtime caller; they
are diagnostics, not an overhead or performance acceptance claim.

The first joint build stops at job 20/149: an older flat-temporal source pin
requires the flush cause inline. The production branch maps the event to a
local cause before choosing its entry. The repaired rig pins the complete
cause-preserving branch and rejects four independent mutants: remapped cause,
broadened selection, missing ordinary restoration and forced fallback cause.
Its focused native gate passes, including all 46 prior corpus frames.

The repaired full tree passes all 149 jobs, config/export contracts and the
self-contained installer checks. The pool takes 237 seconds with no shown
windows, consoles or foreground changes. Its fresh receipt fingerprint is
`2f741c1138238560323f4813762248155fb9a9c11214f05b2480bfe58f589708`. The receipt
is independently verified before committing this tree. No Steam install, live
config edit or flight occurred. Phase 1 performance and whole-ladder/action
coverage remain open; Phase 2 and shipping stay held.

Checkpoint `3c0783e8` is committed, clean-receipt verified and pushed. Fetch
finds new main `d6ecc252`, after three further first-person history/diagnostic
commits. It merges automatically into the feature tree without conflicts;
shared runtime and cause-route test changes coexist. Source review preserves
the pending, owner, context and cause gates and the original NoApi entries. The
checkpoint proof is preserved before rebuilding. Incoming flat identity
readback is outside the partial plugin API ledger; no new coverage is claimed.
Main's offset-shift rescue/diagnostics are built but not yet flown, and the
previous checkpoint flights do not validate them. Steam remains unchanged.

The combined tree passes the flat SDK tool's self-test, all seventeen fresh
scoped assembly gates, all 149 full-build jobs and installer checks. The engine
rig retains 16,279 checks and the updated weapon rig passes 217,576 WARP
checks; the full-classifier composed UI fixture passes. All 46 corpus frames
remain identical. No window, console or foreground change occurs during the
246-second pool. The new full-pass receipt is independently verified:
`203a1657f9c8277c6f0980bfc36ed9361465c08e38adcd4b6bee115949bc7cd8`. The
synthetic selector benchmark reports gating improvement, but it measures only
its fixed typed-selector corpus, not whole-plugin or flight cost. Phase 1
acceptance remains open.

The separate current-only runtime caller listing compiles with production proof
flags without swapping sources. Its first diagnostic parser fails closed on two
indirect jumps (offsets `0xe3` and `0x1e0`); caller-path machine cost remains
unproved while the mid-tier reviewer resolves their actual targets. No
production change follows from that diagnostic failure.

Both indirect jumps resolve to internal image-relative switch tables. The
repaired parser separates 72 table-data bytes from instructions, enumerates
their exact targets and represents the bounded overlay loop explicitly. Eleven
negative controls reject changed bounds/targets/loop and mislabeled return
paths. A body-only pair uses the immutable `f11d6e59` caller inside current
main's surrounding source and headers, compiled sequentially at one ignored
path with identical flags/version. Source prefix/suffix, headers, compiler
input, wrapper and fresh listing pins all verify; tracked source is unchanged.

The caller grows from 672 bytes/167 records to 724/181; stack allocation stays
80 and table bytes stay 72. With overlay inactive, pending-false is thirty
records/94 bytes/no calls in both. Owner refusal is 35/119/two calls, null
context 43/156/two, foreign context 46/174/three in both. Callee order is
unchanged, but exact quiet-path bytes fail equality: register allocation and
return-branch displacements differ. Matching counts are not overhead proof.
OtherDraw's complete caller is one record/five bytes smaller and each other
flush cause one record/one byte smaller, excluding callee bodies. The new EV
boundary still adds its own work to OtherDraw.

The copied current and original-path current have identical encoded bytes and
181 instruction records. Eight raw operand names differ through the compiler's
anonymous-namespace seed (six callees/two data references); these remain
visible and are not normalized away. Exact caller-byte and performance
acceptance stay false. No production compensation follows from these static
diagnostics.

### Night Vision pilot precedence, 2026-10-07

The next bounded gate enables actual registered cockpit operations in the
full-classifier predicate seam, which previously forced dispatch off. Fresh
children compare literal frozen NV hashes/shape, winners, callback tuples and
actions for common foreign context, eye-range preemption, offscreen skip and
fallthrough, the NV claim, a later X6 PanelDistance claim, shader mismatch and
mode-off. The oracle must not derive expectations from manifest helpers. Each
case runs with/without tracing and API sampling. No production capture or hot
path change is needed.

Ruled out: simultaneous NV and PanelDistance eligibility at X240, because the
panel predicate refuses draw counts above 64. X6 instead proves NV's shape miss
falls through to the actual later panel claim. Missing PS b1/b2 makes actual NV
Begin decline replacement safely; this gate targets classification, real plugin
dispatch and forwarding/state restoration. Existing NV rendering rigs remain
the shader/pixel evidence. Live failed-mode coverage stays in the dispatch
matrix. This does not close whole-ladder/actions or Phase 1 acceptance.

The first focused compile fails: the new plain site-capture facade references
fields that NoTrace does not expose. Mid-tier review also finds a V1 window
read beyond its two mask words, action-count reporting after trace shutdown
clears the token, missing explicit eye dimensions and omitted cold descriptor
reads in the Core ledger. These are fixture inputs/oracles, not rendering-fix
hypotheses. The mid-tier repair keeps the plain visitor as NoTrace and observes
canonical sites through a test-only seam. It reads the four-word V2 report,
saves action counts before shutdown, supplies explicit 64x64 eye dimensions and
checks Core's independent descriptor reads (zero for foreign context, three for
ordinary eye paths, six for the offscreen rule).

The repair also restores the existing D-only real-GPU callback guard. NV's X
cases use saved-original typed spies, with start seven, base vertex minus three
and start instance eleven. Actual registered NV claim/Begin/End still execute;
the new fixture makes no NV pixel claim. Mode-off expects zero claim callbacks
because the active mask excludes wantsDraws. Foreign context expects no trace
actions even when capture is armed, because it fails the owner gate. Cleanup
restores borrowed state, clears registry pointers and closes child handles.

Once Elite is observed stopped, the repaired focused predicate rig passes (12.9
seconds), including all 32 NV combinations and six existing UI children. The
eight literal cases each run Trace/NoTrace and sampled/unsampled in fresh
processes. Sampled NV records exactly three CockpitVisuals reads at sites
18-20, other owners remain zero, and unsampled cases record no API notes. The
oracle checks the literal winner, callback tuple, ordered actions and complete
host/binding restoration; its negative controls reject changed expectations. It
does not prove runtime note order or complete route accounting.

Fresh production assembly comparison passes all 17 scoped gates, with all 15
source swaps restored and 17 input pins unchanged. The full build then passes
all 149 jobs, config/export checks and installer-resource verification; its
receipt is `2b5e9829c8ec700d5c241b5dbbd6acc49652366c897e375d735727b06fe655f1`.
The build shows no window and does not take focus. Steam files and settings are
unchanged. Full-ladder coverage, domain/frame/lifecycle API accounting and
performance attribution remain open; Phase 2 and shipping stay held.

### Main PR 80 integration, 2026-10-07

After the NV slice is validated and pushed as `856a3ac3`, the verified remote
advances main to `0d4bc714` (PR 80: idle flat-trace window and late-overlay
extent cap). Sean's main-into-feature authorization applies; that exact commit
merges cleanly, with no feature-into-main action. Mid-tier review finds no
integration repair. The held substitution function body is unchanged, including
pending, owner, exact-context and OtherDraw-only sampling gates.

The runtime assigns trace limits of 4096 while idle and 65536 during active
draw-packet capture or a pending F10 dump. F10 arms at frame F, opens full
recording at F+1 and schedules its dump at F+4; ordinary frame advancement
leaves three completed full-window slots. Serialization remains event-count
based and skips current/truncated slots. The new ring test checks the idle
threshold plus seven overflow attempts and then a full-window busy frame.
Runtime window selection/F10 scheduling and invalid mutable limits remain
upstream test gaps; production assigns only the two valid constants.

Private HDR/mask and replay-depth copies now share a 64 Mi-pixel extent cap.
Format/shape guards remain, and creation failures still refuse. This
intentional budget expansion can admit 576 MiB for an eight-byte HDR plus mask
and 512 MiB for a D32S8 mirror at the cap. Cap equality/over-limit tests remain
an upstream coverage gap, not evidence of a plugin regression. Existing overlay
WARP rigs are part of the full build.

The earlier d6 caller body comparison is historical: this merge changes source,
State/Ring layout and shared-header pins. Its helpers retain strict pin
failures; no diagnostic is relabeled as current and no runtime timing claim
follows from it. A fresh same-header production comparison passes all 17 scoped
gates, restores 15 source swaps and leaves all 17 inputs unchanged. The full
merged-source validation must pass before the merge is committed.

The first full build fails in the predicate rig after 17 of 149 jobs. A UI
child emits the complete expected 196-byte result, but the parent reports
`wait=258`, `exit=0`; the entire rig took 16.1 seconds against a 120-second
deadline. Mid-tier review finds a concrete EOF/process-exit race in both UI and
NV parent loops: pipe close triggers a zero-time process poll, then premature
termination if the process is not yet signaled. Ruled out: a 120-second child
hang, because the failed job ends far before that deadline. The repair keeps
the deadline and strict result/exit checks; no rendering change follows.

A separate mid-tier coverage review rejects a duplicate NV replay proposal.
`plugin_dispatch_test` already compares a literal frozen pre-registry predicate
with candidate dispatch, including earlier claims, modes, failure, shape and
hashes. `draw_ladder_test` writes raw terminal facts; `draw_ladder_replay.py`
independently reevaluates legacy NV, rejects candidate-cache drift and missing
facts, and reports 18/18 NV matches in the existing corpus. All are wired into
the full build. The new 32 children add actual registered cockpit and typed
forwarding evidence, with the existing NV rig retaining pixel coverage. No
additional pilot legacy replay is needed. Whole-ladder/action parity precedes
broader migration; performance and non-regression acceptance remain open.

The repaired UI and NV parents share a bounded child collector. Only
`ERROR_BROKEN_PIPE` denotes EOF; the collector still waits for process exit
within the original 120-second budget. UI's 4096-byte and NV's 512-byte caps,
exact LF/CRLF frame consumption and two-handle inheritance remain unchanged.
Unexpected pipe/wait/termination/exit-query failures reject the child, and
diagnostics include elapsed time, error codes, final wait and PID. The fixture
checks termination and reaping instead of discarding their results.

Three CPU-only controls force EOF while the child remains alive, using an
explicitly inherited test event. The parent releases the child only after
observing that ordering: delayed exit zero succeeds, delayed exit seven fails,
and a long hold is terminated/reaped at a 1.5-second test-only deadline. Their
fixed frame is independently pinned. The focused rig passes in 13.8 seconds,
including these controls and the existing six UI/32 NV children. Fresh assembly
proof then passes all 17 scoped gates. The repaired merged tree passes all 149
full-build jobs, the 236-key config contract, exports and installer-resource
checks in 238 seconds. Its receipt is
`c0266d28dfdcf18bd5f17f16e69de0167f42933f166b1f515b22331cf495fb1c` and verifies
against the exact source tree. No window appears and foreground focus stays
unchanged. Main integration is cut at `0d4bc714`; Steam/settings and the frozen
flight interpretation stay unchanged. Pilot performance acceptance remains
open; broader migration and shipping stay held.

The first clean-version promotion refuses the updated documentation's 61-line
Status block. The fragment helper had omitted the separating blank line from
its count. The Status is compressed, and the ignored helper now uses the
build's own counting function before writing. All 72 Status blocks pass; no C++
source changes and the clean full-build receipt still verifies.

### Raw cost evidence and current caller diagnostic, 2026-10-07

Luna audits the frozen October 7 flight reports without relabeling their
builds. The five-hold normalization pins the three processed inputs and reports
both occurrences/frame and estimated cost/occurrence. Carrier DLSS/NV-off EDVR
cost rises 0.170794 ms/frame: two upscaler calls/frame remain constant while
their estimated cost rises 0.118731 ms/frame; UI helper attempts rise 22.9% and
their estimated cost rises 0.062104 ms/frame. Helper attempts are not admitted
redraws or matched sample cohorts. The existing three-decimal rows lack
per-scope raw sample totals and null calibration. Normalization cannot assign
causality or establish non-regression; changing ship workload remains a
plausible confounder. The report's self-test and read-only dry run pass. No
additional useful normalization of these frozen reports is available.

Luna refreshes the ignored static caller diagnostic against the merged 814
tree, preserving the d6 artifacts as historical. The immutable f11 caller body
is substituted into identical current surrounding code with shared header,
prefix/suffix and roundtrip pins. Both native compilation stages succeed. The
first comparator fails by subtracting return-location lists; mid-tier repair
compares their lengths and adds signed/reversed/equal/moved-location controls.
Those checks and report generation pass. The current body gains 52 instruction
bytes and 14 records, with 95 raw instruction-row and 36 raw CFG differences;
stack and switch-table byte counts match. Raw equality gates fail. This is a
body diagnostic within current surrounding code, not a frozen full-hook cost
benchmark. It explicitly keeps performance acceptance false. Mid-tier review
also rejects a predicate-only synthetic timer as a whole-hook comparator,
because it omits callers and measures extra setup.

Luna adds cold per-scope v1 rows at the existing census window close, before
reset. They expose completed timestamp sums/counts, helper occurrences, frames,
raw owner/scope/attribution IDs and the existing shared null cohort. No query,
rotation, cap, timer, hot-path operation or setting changes. Unmeasured,
uncalibrated and null-floor states remain explicit; late completions may have
zero current-window occurrences. Invalid deltas or zero-count positive sums
produce rejectable diagnostics instead of fabricated zeros. Mid-tier review
finds that fixed-decimal rounding can erase a just-above-floor difference;
17-digit roundtrip formatting fixes it and literal near-floor/tie cases pin it.

The sanctioned log reader gains `--plugin-cost`, after build-identity checking.
Its strict parser rejects malformed/future schema, incoherent windows,
duplicate scopes, inconsistent status and any invalid writer diagnostic. No
rows exits with no instrument evidence. Unknown logical IDs remain raw, and the
output labels estimates as sampled-helper cost, not accepted-work latency. The
complete reader self-test passes. The focused native census passes 344 checks;
fresh production codegen passes all 17 scoped gates. The initial focused
wrapper lacked the required CFLAGS environment and refused to run; the
corrected ignored wrapper passes. The full changed-source build passes all 149
jobs, the 236-key config contract, exports and installer-resource checks. Its
receipt `5bea0bcf318bf78874e40388712d5f5b9a6c945567eb6e516f8c7ff0deaad6ae`
verifies against the exact source and compiler context. No window appears or
foreground focus moves.

The ignored cross-language diagnostic reads the actual focused rig output,
explicitly not a flight log. All 70 normal writer rows parse singly both with
and without a real-format timestamp prefix (140 parses). All nine invalid
diagnostics fail reader mode in both forms (18 checks), covering all four
statuses and all eight reasons. It records the exact fixture hash and counts.
Its self-test and dry run pass, with the existing report unchanged by dry run.
This proves the writer/reader contract, not GPU performance.

Mid-tier review approves a matched telemetry-only control on the complete
immutable `0d4bc714` tree. Only `gpu_census.cpp`, `gpu_census.h` and its rig
are backported, with exact final SHA-256 pins; baseline scanner literals and
build dependencies already match. Every other tracked file stays at main. The
managed control worktree receives the same 50 cached dependency files with byte
verification and its own `codex/plugin-telemetry-control-0d4` branch. The
feature reader will read its logs externally; no feature Python tool is copied
into the control. Its full absolute build passes all 129 jobs, the 235-key
config contract, exports and installer-resource checks. Receipt
`e83b9268ba5bfa1a20dcec36ac05ea7e8ff3950b35ebebfd6fbda2a039267486` verifies
against the exact source/compiler context. Commit `405b14cd` has only the three
reviewed census files and parent `0d4bc714`; its clean receipt verifies. Both
clean-version DLL promotions pass, for candidate `2bd15a67` and control
`405b14cd`, and both testing branches are pushed with remote hashes confirmed.

The remote advances main to `6b43ffc9` during validation. The complete delta
from `0d4bc714` contains only two Explorer Cam docs, with no build-input
change. That exact commit merges cleanly into the feature branch under Sean's
standing authorization; the production main cut and control stay identical to
`0d4bc714`. A final candidate version promotion follows the documentation
merge. Steam/settings stay unchanged at this point. GPU attribution and Phase 1
acceptance remain open; broader migration and shipping stay held.

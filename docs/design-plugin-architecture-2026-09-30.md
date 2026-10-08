# Plugins: every fix a selectable module (design, 2026-09-30)

## Status

- **State:** design only, for the release after v0.18.0. Nothing is built.
  It extends draft PR #46 (Devin Nemec, "generic OpenXR addon and plugin
  architecture"), which this document reuses as its add-on tier (section 6).
  The runtime-dependency check is done (section 4.1).
- **Goal (Sean):** every fix and performance item belongs to one plugin,
  plugins group features logically, and the user picks which to install in
  the installer. A plugin that is not installed costs nothing.
- **Built on:** a read-only inventory of main `4296f142` (25 features, 225
  ini keys, 19 draw verdicts, the services they share; section 2), the flat
  CPU census of 2026-09-30 (hook entry about 1.4 ms over 17,180 calls a frame
  on foot; D3D call counts are what a wrapper such as ReShade multiplies),
  the landing-pad regression of 2026-09-29 (a claim order nobody had written
  down) and PR #46's diff.
- **North star (Sean, 2026-09-30): performance.** The plugin layer never
  makes EDVR slower, and its first phase makes it faster. Gates are relative:
  each phase flies the same spots and must be no worse than the previous
  phase, within noise (section 7).
- **Decided, Q1 (Sean, 2026-09-30): monolithic.** First-party plugins are
  modules inside the one DLL: each a static library behind the plugin
  interface, a build gate against one plugin including another's internals,
  and an unselected plugin never registers. The interface stays C-compatible
  so a split into DLLs stays mechanical (section 10 has the reasons).
  PR #46's C ABI stays the add-on tier, moved into the core so flat mode
  gets it too.
- **Decided, Q2-Q6 (Sean, 2026-09-30):** the nine plugins of section 4
  (regrouped: intro and on-foot-panel split out, UI quality inside
  temporal-aa); defaults that reproduce today's shipped behaviour; the
  add-on tier after Phase 1 (PR #46's OM-unbind fix lands on its own now);
  diagnostics probes out of the default install, census kept in the core;
  today's keys and sections kept, each owned by a plugin. Section 10.
- **Decided, Q7 (Sean, 2026-09-30): graphics-only VR is its own phase,
  after Phase 1.** When no selected plugin needs the OpenXR runtime (section
  4.1), the installer skips it and Elite stays on its stock VR path. Four
  changes first (section 10); no F8, AA, flash fix or Explorer Cam without
  the runtime; the first build needs a flight on a stock runtime.
- **Next:** Phase 1 (section 8), after v0.18.0: the registry and dispatch
  tables in the core and night vision moved behind them, gated by a
  byte-identical verdict replay and a lower render-thread census. The
  registry also owns the draw-gate subscriptions and takes static props and
  the scheduler probe off temporalPassConfigure.
- **Removed 2026-10-08 (branch claude/remove-settlement-detail):** static
  props (`fix.static_prop_updates`) and settlement detail
  (`fix.settlement_detail`) no longer exist; the plugin tables below that list
  them are the 2026-09-30 design, not the code.
- **Ruled out while designing:** loading every DLL found in a folder (DLL
  planting; the installer's receipts already know what it installed), a
  stable ABI for first-party plugins (they ship with the core; freezing
  their interface buys nothing and costs every refactor), and a per-draw
  virtual call into every plugin (section 7's measured costs forbid it).

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

# Native on-foot VR: ship camera and mission-menu handoff

## Status

- Take over the ship-selection camera and mission-completion menu fixes.
- Latest installed code: `22cabf3c434fa0fc42817e841efca30433a8886f`,
  `v0.22.0-rc.1-5-g22cabf3c`. Both Frontier profiles were installed and
  verified; existing settings, including detail logging, were preserved.
  Installation was reverified while preparing this handoff.
- IMPORTANT NEW EVIDENCE: a newer flight was found while preparing this
  handoff. `edvr_gfx_20261026_101921.log` matches that code. The camera
  correction installed and applied 2,532 times, with zero reported refusals.
  Its visual result has NOT been reviewed here or reported by the user.
- New menu captures now contain both authored mesh pairs: 77 vertices / 120
  triangles and 4 vertices / 2 triangles, with successful capture, model and
  map status. This is metadata inspection only: alternate geometry, placement,
  GPU association and input behavior still need analysis.
- Start with those newest dumps, not another diagnostic build. Their stamps are
  `102137780001`, `102149135002`, `102221594003`.
- Mission flattening has NOT been implemented. The previous flight exposed an
  unreachable camera hook and a premature menu capture filter; both are
  repaired in the installed code.
- Leash is user-confirmed good: horizontal fade/black 25/35 cm, upward 5/10 cm.
  Leave it alone. Sniper ADS release and bracer visibility are user-confirmed
  fixed in `ccdf75ef`; do not reopen them without evidence.
- Full Windows validation passed in 213.1 seconds; clean DLL promotion passed
  in 41.1 seconds. Source is merged and pushed to main.
- A later docs-only commit may contain this handoff. Compare flight versions
  against `22cabf3c`, not blindly against HEAD.
- Authoritative investigation journal:
  `docs/design-native-onfoot-vr-2026-10-13.md`. Read its Status first, then the
  final October 26 entries. It is very large: never dump it whole. This handoff
  adds the newest flight metadata, which the journal has not yet incorporated.
- Next action: analyze the new captures and obtain/inspect the visual result
  before deciding whether the ship camera needs further changes. For the
  mission menu, establish the alternate surface's geometry and shared
  rendering/input selection contract before editing production behavior.

## Workspace and operating rules

Main checkout: `C:\Users\seanm\projects\edvr-unofficial-patch`

Existing active worktree:
`C:\Users\seanm\projects\edvr-unofficial-patch\.claude\worktrees\native-onfoot-vr-completion-219ffa`

Branch: `codex/native-onfoot-rc1-followups`. Reuse this worktree where
appropriate. Its `build/rc1-followups/` contains prior analyses, images,
focused checks and full build logs. Those ignored artifacts are not in main.

Frontier game root:
`C:\Users\seanm\AppData\Local\Frontier_Developments\Products\elite-dangerous-odyssey-64`

Read root `AGENTS.md`. Use PowerShell and sanctioned tools. Here, `--target
frontier` fails discovery; use the explicit game path. Preserve both live INIs
and the hook/layout guards.

The separate private analysis repo is
`C:\Users\seanm\projects\edvr-unofficial-patch\analysis`. Read its AGENTS
before using it. Worktree sessions may read it but must not write, clone around
the guard, or junction it into a worktree. Use worktree `build/` for scratch.
Game binary for this arc: `analysis/EliteDangerous64-334925.exe`.

Rendering context: build 334925, Pimax Crystal Super using Pimax OpenXR. Prior
verified runtime recommendation 4508x4510 per eye, scale 0.894410; captures
render 2016x2017 and output 4032x4034, DLSS Performance preset K, 90 Hz.
Recheck current runtime/settings when they matter. DLSS binary version was not
established; do not invent one. Camera writes deliberately require exact known
build 334925 even though resolver maps cover others.

## New flight: inspect this first

Correct graphics log: `edvr_logs/edvr_gfx_20261026_101921.log`, build
`6ADF7B45`, linked 2026-10-26 16:09:41 UTC.

At 10:22:02.864, preview calls, both producer qualifications and applications
all reach 1. At 10:22:33.695 they reach 2,525. At 10:22:42.170 the active flag
goes off, with 2,532 applications and zero refusals. Installation succeeded.
This proves the correction ran, not that its composition or the final displayed
view is correct.

Companions live in `edvr_logs/pool/`:

| Stamp | Selected menu frame | Surface evidence |
| --- | --- | --- |
| 102137780001 | 8681 | One typed owner, paired path; both meshes retained |
| 102149135002 | 9259 | No owner callback; no authored surface rows |
| 102221594003 | 11567 | One typed owner, paired path; both meshes retained |

Each stamp has `camera_<stamp>.json`, `menu_<stamp>.json` and
`menu_<stamp>.eyemesh.bin`; inspect other draw/eye companions as needed. Images
are under `edvr_logs/eyes/`, notably `eye_<stamp>_FinalCrisp_LOverview.bmp`.
Identify the scenes visually; the latest three images were NOT opened while
preparing this handoff.

For the first and third captures: `input_observer.surface_evidence` in the raw
menu JSON reports callback 1, type_declined 0, non_menu 0, quota 0,
crossed_frame 0, layout_declined 0. The row has status 4, path 1, map_status 4,
readable pre-rebuild count 0. Both pair, model and mesh statuses are 4. Mesh
payloads hold 385/20 floats (float3 XYZ + float2 UV, hence 77/4 vertices) and
360/6 byte indices. These counts alone do not prove flatness or rendered
ownership. The combined reader's wrapper uses a different nesting path from raw
JSON.

Useful initial commands, run in the active worktree:

```powershell
$game = 'C:\Users\seanm\AppData\Local\Frontier_Developments\Products\elite-dangerous-odyssey-64'
python tools/edvr_log.py --target $game --expect-build 22cabf3c --version
python tools/edvr_log.py --target $game --expect-build 22cabf3c --grep 'native preview camera:|CodeHook native-preview'
python tools/edvr_log.py --target $game --expect-build 22cabf3c --health
python tools/eye_bmp_to_png.py --help
```

Use the image conversion tool's dry-run before writing. Existing metadata
parsing/validation lives in `tools/eye_draw_snapshot.py`; inspect its menu
reader and `validate_menu_surface` before inventing a parallel decoder.

## Ship-preview camera: proven cause and current correction

User symptom: selecting a ship on foot places the view inside its hull,
apparently near the ship's origin. Do not interpret the user's “0,0,0” as a
measured world-space coordinate.

Flight on `ccdf75ef`, ship dump `081716266003`, established:

- Location stage 15 is mode 0 with HMD first at weight 1 and
  CustomisationVisual second at constant weight 1.
- The finalizer keeps the first equal-weight activity. HMD output is
  `(-621.280762,-26.8717365,-78.841095)`; renderer/GPU midpoint agrees within
  98 micrometres.
- Valid CustomisationVisual origin is `(-615.077515,-64.7949448,-21.0558281)`,
  69.396 m away.
- Disabling HMD would lose tracking. Reading the preview inside the HMD
  producer is unsafe: sibling producers may run in parallel.
- The exact local HMD pose is retained by the game in secondary camera rows; a
  cached runtime-pose approximation had up to 1.19 degrees residual.

Sources:

- `src/d3d11/native_preview_camera.cpp`: four hooks, installation, gate, safe
  memory access and logs.
- `src/d3d11/native_preview_camera_core.h`: ownership, evaluation pairing,
  producer qualification, finalizer composition.
- `src/d3d11/native_preview_camera.h`
- `src/d3d11/native_on_foot.cpp`: boundary after `cameraGateTick`, active
  native-on-foot gate and external-camera exclusion; shutdown.
- `tools/flat_stationary_camera_test/native_preview_camera_test.cpp`
- `tools/flat_stationary_camera_test/native_preview_install_test.cpp`

Static landmarks (build 334925):

- HMD update `0x1074140`: concrete A, interface A+0x20, child A+0x140. Primary
  state A+0x70. Exact service pose is fetched via vslot 0x60 at `0x10742CA`,
  then stored unmodified at A+0xF0..0x120. Secondary pose is returned
  CameraState+0xC0.
- Preview callback `0x1261680`: concrete interface+0x60, task subscriber
  +0x1B0, child +0x180; mode +0x39C. Unless mode 2, current callback copies
  selected pose +0x4D0..0x50F to primary +0xB0..0xEF and sets FOV.
- Finalizer `0x1070980`, manager `0x106D8E0`, synchronous dispatch `0x7EB6A0`,
  priority wait handler `0x7FB450`.
- Registered uniquely owned children and priority fences establish the
  exclusive finalizer callback interval. This does not support invalid
  concurrent game dispatch of the same child.

Current correction pairs successful current producer completions within a
manager evaluation (16 child slots), validates exact owned Location roster and
current identities/poses, consumes once, and composes exact local HMD with
preview primary. It writes four 12-byte XYZ rows only. W lanes, FOV, secondary
state, projection and all other fields are preserved. Originals run exactly
once. Nonfinite XYZ, stale/ambiguous/changed inputs refuse. Unused nonfinite
fourth lanes are allowed.

Prior review fixed an owner-publication race: owner CAS is the admission point.
Different producers may overlap; duplicate same-owner work poisons the
evaluation. Commit admission rejects observed overlap before reservation. Do
not replace this with an unsupported “atomic 64-byte pose write” claim.

Failure in flight `24a2b567` at 09:46:10.176: `native-preview-hmd` direct
five-byte jump could not reach the DLL callback over 2 GiB away. Installed=0
and all counters zero. Camera composition was not exercised. `22cabf3c` fixes
this with local RX near relays; successful hooks/relays remain alive after
disable/stop/partial install. Only a failed, unpublished relay is freed. The
newest flight confirms this fix works.

Before changing camera math, compare newest camera/GPU eye data and images,
including headset-local motion and downstream writes. An application count
alone is not end-to-end visual proof.

## Mission completion: required behavior and remaining proof

User wants a flat panel, somewhat smaller, ideally fitting the physical
terminal. In flat mode the game presents this menu flat across most of the
screen. Both mouse and keyboard/controller navigation must work.

Already established:

- Current curved GPU surface has 77 vertices / 120 triangles. All CPU picking
  triangles match it by UV/topology; rigid fit 2 micrometres RMS, owner
  transform within 6.2 micrometres.
- It uses its own curved input owner, not the physical terminal's picker.
- Mouse/controller coordinates become movie UV before geometry lookup. Preserve
  UV selection, visible 3D cursor position and ray-mode geometry together. Idle
  captures do not establish active click accuracy.
- Current update/placement callbacks occur after the open-menu GPU draws; they
  cannot authorize a fresh pre-draw mode/owner decision.
- Mode interface belongs to a heap-owned CursorComponent, not a stable global.
  Never dereference a prior callback's pointer on the draw thread. Placement
  mode may be unavailable (-1); do not assume mode 0 forever.

Promising shared route, not yet implemented:

- Selector `0x452CFF0` chooses two authored handles depending on camera mode
  3/4. Visual provider `0x450FFD0` uses pair+0; pick provider `0x452D320` uses
  pair+8.
- FullPanel+0x868 references the paired entry; entry+0x18 points to owner. A
  missing entry uses the map's first pair.
- Constructor path `0x4502500` creates model and CPU mesh from one definition
  and inserts their pair under a resource key. This associates the pair, but
  does not prove which actual GPU draw used it.
- FullPanel vtable `0x55DBA08`, owner interface `0x55DBA10`; concrete model
  vtable `0x55999C0`, constructor `0x434F600`. Model +0x10 basis, +0x50 scale;
  +0xF0 and +0x150 are labelled constructor identities, not presumed asset
  semantics.
- Map is a null-terminated binary tree: root is an actual node, left+0,
  right+8, uint16 key+0x20, pair+0x28. Do not skip the root or treat it as a
  sentinel.

Capture implementation: `native_on_foot_menu_surface.h`, existing
owner-transform callback in `native_on_foot.cpp`,
`native_on_foot_menu_input.h`,
`tools/eye_draw_snapshot_test/menu_surface_checks.h`, and reader above. It is
manual-eye-capture only: one selected complete frame, at most four typed
owners, 255 triangles/256 vertices per authored mesh, 32-node walks, try-lock,
generation and identity checks. No new getter calls/game writes. Raw JSON
explicitly says `gpu_draw_owner_proven=false`.

Failure in `24a2b567`: requiring cached count 120 inside owner callback
rejected the menu. Mesh refresh `0x45412E0` clears count first, calls owner
transform at +0x4D, then populates triangles at +0x150 onward. `22cabf3c`
records the valid zero callback-time count, copies authored pairs there, and
keeps the completed post-placement mesh as separate evidence. The newest
captures confirm paired data is now present.

Next: inspect alternate XYZ/UV geometry and model basis/scale, join current
pair to captured CPU/GPU geometry, then prove a selective shared-provider
change keeps rendering, cursor and picking coherent. Do not ship GPU-only
flattening, force a global mode, fit an arbitrary terminal rectangle, or add
another diagnostic flight before using the newly available pair data.

## Prior evidence and validation

Historical captures:

- Earlier artifacts under `flight3/` and `menus-flight3/` establish the menu
  UV/CPU/GPU geometry match. ADS/bracer fixes landed in `ccdf75ef` and were
  confirmed by the pilot on the following flight.
- Flight 4 log `edvr_gfx_20261026_080951.log`, stamps `081642413001` mission,
  `081653968002` closed, `081716266003` ship.
- Flight 5 log `edvr_gfx_20261026_094506.log`, stamps `094721440001` mission,
  `094739603002` closed, `094808485003` ship, `094829429004` closed.
- Worktree `build/rc1-followups/flight4/camera/` holds static camera analyses
  and resolver results. `flight4/menu/` holds capture validation.
  `flight5/overviews/` holds inspected images; `flight5/health.txt` is the
  previous flight's health, not the new 10:19 flight.

Installed code validation:

- Preview wrapper fixture: 163 checks. Real distant-target installer: 60.
  Covers original-once/results, all four relays, RX pages, partial refusal,
  unavailable module/build, pin refusal and passive stop.
- Actual menu writer/parser rig passes reset0 -> callback copy -> populate120
  -> separate completed mesh, quads, quota, unreadable counts, old JSON and
  malformed input.
- Combined resolver maps: 328 targets, known and both historical builds; 79
  checks, no wrong/unresolved mappings.
- `build/rc1-followups/flight5/full-native.log`: all gates, 213.1 s.
- `build/rc1-followups/flight5/promote-native.log`: clean promotion, 41.1 s.

Follow AGENTS for full validation, commit, merge/push and clean DLL promotion.
Set `EDVR_XESS_SDK` to the main checkout's `third_party\xess`. The machine-wide
build lock is shared with other worktrees; do not bypass it. Install Frontier
with `--scope both --dlss --xess`, dry-run then actual then `--verify-only`,
without `--ini`. No new RC tag or release is requested.

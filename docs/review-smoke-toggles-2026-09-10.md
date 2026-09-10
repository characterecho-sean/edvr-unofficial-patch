# Latest smoke run and incomplete effect switches, 2026-09-10

**Follow-up:** the [12:02 captures](review-smoke-capture-1202.md) confirm
the logging improvement and the still-visible trails. They also lead to
removing the misidentified planetary shader from the smoke switch. The
installed-state and next-capture descriptions below refer to the earlier
11:14 review and first takeover build.

Reviewed the other worktree through `794ee65` and continued from that commit
on `codex/smoke-temporal-history`. The station origin fixes are retained.
The rendering changes through `627d62f` are also retained; that is the code
identified by the latest Steam log. This follow-up supersedes the earlier
review's description of smoke writing directly into the game's DSV.

## What the latest run establishes

The Steam install is
`C:\Steam\steamapps\common\Elite Dangerous\Products\elite-dangerous-odyssey-64`.
The sources for this review are its `edvr_logs` files:

- `edvr_gfx_20260910_110551.log`, ending at 11:14:25.
- `edvr_vr_20260910_110605.log`.
- The `eye_111407` images and associated `draws_111407.bin`, pool, instance,
  palette and auxiliary captures.

The graphics log identifies `v0.14.1-159-g627d62f`. It reports the private
smoke depth target engaging at 11:08:23. The build contains the depth-target
isolation and the reconstruction from the smoke vertex shader's view-depth
output. Those fixes therefore were present in this run.

`fix.drives_smoke` changes to `off` at 11:09:27. Its withheld-draw counter
continues increasing, reaching 155,828 at 11:13:57. Heat haze is automatically
withheld while temporal AA is active, and is explicitly set to `off` at
11:12:05. The haze counter reaches 126,280 at 11:12:33; the sampled binding
shadow audit reports no disagreements. This is evidence the settings reach
their skip functions and suppress matching draws. It does **not** prove
they suppress every visible smoke or distortion pass.

The run includes native TAA from 11:10:31 to 11:12:43, followed by DLSS.
The eye dump at 11:14:07 was therefore captured under **DLSS with both
effect switches off**. Its sixteen center crops are raw inputs to the
temporal pass; `L0` is the first treated whole eye. They show a station
view, without a sufficiently isolated trail to identify the reported
rectangles by eye. The single treated image cannot establish their
temporal behavior.

The ledger has 29,387 rows across nineteen populated frames, with crops
corresponding to frames 48277–48292. None of the known two smoke or three
heat-haze vertex shaders appear in those rows. However, the log reports
2,707 substituted plume draws in the ten seconds ending 11:13:58 and
3,412 in the interval ending 11:14:17. The plume shader is
`EB787F983BC1F5A3`, shared by general smoke/steam particles. Those draws
returned from `beginPanelOverride` before **both** the ledger and the
normal census calls. Their absence from the ledger is an instrumentation
gap, not evidence they were absent on screen. No full text census was
recorded alongside this latest eye run.

## Confirmed source defects and changes

1. `drivesSmokeSkip` previously accepted only indexed-instanced draws,
   exactly one instance and 300–16,384 indices. `heatHazeSkip` accepted
   only indexed-instanced draws, nonzero instances and counts divisible
   by 36, capped at 4,096. These conditions could let a recognized effect
   through when drawn using a different API, count or instance count.
   Both switches now match their existing shader identities without
   those additional shape gates. Haze retains its periodic context audit.
2. `DrawIndexedInstancedIndirect` and `DrawInstancedIndirect` never called
   either switch. Both hooks now suppress recognized effects on the
   context owning the binding shadow. Foreign contexts still forward,
   and an armed census still records the submitted indirect call before
   suppression. GPU argument buffers do not need to be read back.
3. Visible substituted billboards now enter the draw census and eye
   ledger before the replacement is bound. The census uses the existing
   `DC`/`DCO` record formats and resource details. The ledger retains its
   binary format and records the original VS and start-instance value.
   Diagnostic work occurs only while the corresponding capture is armed.
4. An eye run with `advanced.object_probe` enabled now also arms a full
   census including offscreen work, unless a census is already pending or
   active. Existing census length and line limits still apply. The user's
   installed limits are two frames and 16,384 lines.
5. Census summaries now count heat-haze and drive-smoke early skips in
   their own buckets; previously those reasons were mislabeled as FSS
   chrome skips.

No new effect shader hashes were added. In particular, the general plume
shader has not been assigned to the ship-smoke switch: it also renders
non-ship effects. The preceding volume-like pass `B12F7A618E1BDE98` seen in
an earlier census is another candidate to investigate, not a proven ship
smoke identity. No additional TAA depth, motion or history changes were
made in this follow-up.

## Validation and remaining uncertainty

`tools/check_drive_switches.py` compiles the actual skip functions,
indirect hooks and particle early-return block against context doubles.
It tests known smoke/haze identities over direct and indirect APIs, zero
and varied counts, multiple instances, independent switches, foreign
contexts, context audits, and capture routing. Unknown, plume and flare
identities remain unsuppressed. Against `794ee65` the regression cases
report 577 failures; the changed source passes all 3,895 checks. The
script is part of `build.bat`.

The final build used the verified pinned SDK at
`C:\Users\seanm\AppData\Local\EDVR\ngx-sdk`, with DLSS support and runtime
included. The full build's checks and `build/smoke.exe build/d3d11.dll`
passed. The first development build lacked SDK access inside the sandbox;
it was not installed. The SDK-enabled build was compiled after granting
access and is the one verified and deployed.

Installed both patch DLLs as `v0.14.1-160-g794ee65-dirty`, with SHA-256
verification against the tested binaries. The source changes remain
uncommitted on the takeover branch. `edvr.ini` was unchanged. Backups and
the deployment manifest are in the worktree's
`build/review_motion/takeover-backup-20260910`; build and GPU test logs are
`takeover-build-ngx.log` and `takeover-smoke.log` alongside that directory.

These tests prove the code gaps and their correction, **not** that those
gaps caused the remaining rectangles in the latest flight. The available
capture cannot distinguish an additional exhaust shader from another
particle effect or temporal corruption of that effect. Rising skip
counters should not be presented as visual confirmation that the smoke
has disappeared.

The next useful capture is an eye run with the artifact clearly visible,
using the temporal mode in which it occurs and leaving both effect
switches off initially. The eye-run key now supplies the accompanying
census automatically when the object probe is enabled (as it is in the
installed INI). That capture can name the surviving draw's original
shader and resources before deciding whether to extend effect recognition
or change temporal processing. Runtime visual confirmation is still owed.

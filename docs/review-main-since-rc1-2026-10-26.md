# Review of main since 0.22.0 RC1 (2026-10-26)

## Status

- Scope: `v0.22.0-rc.1` (`b7c1cc43`) through `0a3762d3`, including the flown
  preview/seat work and PR #103. Fixes are based on `0a3762d3` in
  `codex/native-onfoot-rc1-followups`.
- Confirmed findings: stale preview use after disable, stale menu mesh reuse,
  an unchecked change in the captured indirect mode value, and skipped
  validation of release-branch PR merge candidates. The full build also caught
  a missing archived-build fixture entry. All five fixes pass their focused
  regressions, and the full native Windows build passed with exit code 0.
- No new correctness defect found in ADS acceptance/release, bracer visibility,
  leash limits, seated height, journal state or runtime recentre handling
  during this pass.
- Still open: the mission-completion panel and mouse alignment. This review
  does not flatten the panel. Manual eye dumps also retain a synchronous
  index-buffer readback described below.
- Next flight: exercise preview entry/exit and native-mode off/on, then capture
  the mission menu using both mouse and keyboard/controller input. No headset
  flight, install, RC2 tag or release was performed for this review.

## Findings and fixes

### High: a disabled preview hook could overwrite the game's eye pose

`Engine::enable(false)` poisoned manager state but left the published eye
serial live. `loadEye` did not check whether the feature remained enabled. The
installed eye hook could therefore replace a valid raw HMD pose with the
previous ship-preview pose after the native-mode boundary went inactive.

The installed-wrapper regression failed on the unfixed code: the raw pose
changed after disable. It now checks a real pose, original forwarding exactly
once, and an inaccessible pose pointer that must remain unread while inactive.
Core cases also cover a newer evaluation, poisoning, repeated active boundaries
and disable/re-enable. The fix invalidates publication and checks its
evaluation and generation. A nonblocking claim protects the full pose snapshot;
contention falls back to the game. Actual overlapping game jobs were not
established, so the payload race is not counted as another confirmed flight
defect.

A second installed-wrapper regression disabled preview during the game-pose
read, after the snapshot had been copied. It failed until the snapshot's epoch
and generation were rechecked immediately before the write. The same test
covers a newer manager evaluation at that point. A write admitted before a
concurrent disable remains in flight; this does not make a multiword game pose
store atomic with the mode toggle.

Sources: `src/d3d11/native_preview_camera_core.h`,
`src/d3d11/native_preview_camera.cpp`; regressions in
`tools/flat_stationary_camera_test/native_preview_camera_test.cpp` and
`native_preview_install_test.cpp`. Review rules: read/write interleaving and a
failing regression before the fix.

### Medium: menu captures could attach old geometry to a new placement

`Capture::finish` reused a mesh by owner, panel, list, frame and triangle
count. A replaced buffer or an in-place rebuild with the same count reused the
earlier triangle payload and reported success. That could mislead the
mission-menu investigation even though rendering itself was unaffected.

The regression replaces a 120-triangle buffer, then edits it in place. It
failed before the fix because all three placements shared the old payload.
Reuse now requires the same buffer address and bytes. Bounded callback-time
reads reject geometry that changes while copied; the parser checks the recorded
buffer identity. The four-mesh-per-kind cap is unchanged. The maximum 160
retained placements add at most about 7.5 MiB of memory reads to one manual
capture; there are no later reads through saved game pointers.

Sources: `src/d3d11/native_on_foot_menu_input.h`, `tools/eye_draw_snapshot.py`,
`tools/eye_draw_snapshot_test/menu_input_checks.h`. Review rules: list reads
interleaved with writes, and a failing regression before the fix.

### Low: an indirect mode value could change during its verified read

`bridgeMode` rechecked the object, vtable, getter and value pointer but not the
mode value. A value change during that sequence still received the
verified-read status. This is a diagnostic consistency issue, not proof that
the game executed the wrong input mode.

An injected change after the first value read reproduced the gap. A second
bounded value read now makes that observation unavailable when it disagrees.
Sources: `src/d3d11/native_on_foot_menu_bridge.h` and the menu-input rig above.
Review rules: read/write interleaving and claims supported by observations.

### Medium: release-branch PRs could skip every validation job

The workflow assumed a `codex/release-*` push had already built the PR's
candidate. A push builds its head; a PR normally tests its synthetic merge with
the current base. Both `build` and `light-gates` were skipped by the exemption,
so a different merge candidate could receive passing skipped checks. GitHub
documents the [PR merge
ref](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#how-the-merge-branch-affects-your-workflow)
and [skipped required
checks](https://docs.github.com/en/pull-requests/how-tos/merge-and-close-pull-requests/troubleshooting-required-status-checks#handling-skipped-but-required-checks).

The workflow's own Decide script reproduced both skips with a release-branch PR
fixture whose base was `6b8ef374` and head was `b3be6ab6`; the actual merge
`0a3762d3` differs from that head. Removing the exemption selects the full
build. Docs-only PR and push controls still select light gates and the full
build respectively. Evidence is under `build/rc2-review/ci/`, including the
extracted scripts and their outputs. No real protection API request was made.

Source: `.github/workflows/release.yml`. Review rules: test every route and
reproduce external-system claims. The protection proposal also labels its
command as Bash and distinguishes its historical rationale from current CI.

### Medium: the archived-build fixture omitted a new camera target

The first full native build failed `game_build_test`: the 332841 fixture had
328 expectation rows for 329 signature targets. Its missing eye-producer row
shifted the later comparisons, producing 48 apparent wrong addresses.

An independent `build_diff.py map` lookup returned RELOC-ONLY for the missing
target. A newly generated signature table resolved it to the same location.
That ground truth was supplied to `hook_signatures.py check --emit-expect`,
which regenerated the missing row and its three in-unit probes. The focused rig
then passed all 79 checks: 329 equal targets and zero wrong addresses on
332841, with all C++/Python answers agreeing. The 334473 controls also passed.
The production signature table was unchanged.

Source: `tools/game_build_test/expect_332841.txt`; independent mapping and
fixture regeneration evidence under `build/rc2-review/`. Review rules: proven
claims, lookup ownership, and a failing regression before the fix.

## Validation and limits

Focused camera and capture regressions have passed after failing on their
unfixed implementations. Camera core: 211 checks, zero failures; installed
camera hook: 82 checks, zero failures. The first full native build compiled the
production DLLs but failed the archived-build signature rig: the 332841 fixture
had 328 expectation rows for a 329-target table. After its repair, the full
native Windows `build.bat --wait-lock` completed with exit code 0 in 174.0 s.
It built the production DLLs and self-contained installer and passed all Python
gates, C++ rigs and installer-resource checks. The eye-dump C++ writer, Python
reader self-tests and fixture verification passed. Status-block and
agent-material checks also passed, as did `git diff --check`.

Validation ran on `0a3762d32c2bfaec00f8faa594fbd23d8f810df6` with this review's
changes present (`v0.22.0-rc.1-26-g0a3762d3-dirty`). The full-pass receipt's
input SHA-256 is
`4411b7dd3d59e4760528abbd1abab6d1de8b338251b7505e0b23d4e45dbc7111`. Logs:
`build/rc2-review/full-native.log` preserves the initial failure;
`build/rc2-review/full-native-2.log` records the passing run. The workflow
Decide script was executed locally; hosted CI was not used as this candidate's
validation path. No headset flight, packaging, install or RC2 publication was
performed.

The manual eye-dump path can synchronously map up to 48 bounded index windows
for late-menu rows. This uses the existing F77 capture path. A nonblocking map
could discard the exact vertex window needed by the investigation; the fixture
checks that window. No duration measurement established a flight regression, so
no readback change is included. A future performance change needs timing
evidence and a design that preserves pre-draw vertex bytes.

The rendering changes remain tied to Elite build 334925. Offline rigs do not
establish headset appearance or menu input alignment. The user confirmed ADS,
bracers and the wider leash; `b3be6ab6` records the preview and seated behavior
as flown. This review adds no new runtime/headset/render-size evidence.

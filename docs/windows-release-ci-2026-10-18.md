# Windows release builds from the Linux device

## Status

- State: full native Windows validation and packaging passed in run
  `39134563542`; the downloaded receipt and all three ZIP checksums were
  verified on the Linux device. GitHub CLI authentication is working.
- Latest failed run: `38990816615`, source
  `e700f005ef390e1c17c207f422c9118784209948`.
- Actual failing gate: `flat_mono_resolve_shard2`, specifically the final
  Python motion-fixture verifier. The C++ suites reported PASS.
- Confirmed cause: separate `FlatDrawCapture` instances reused the same
  timestamp/PID/per-instance serial directory. A production-header no-GPU
  harness reproduced the collision; a process-wide atomic suffix fixes it.
- Ruled-out pointer: the seven first-person `FAIL:` lines belong to mutation
  children whose failures are required; their parent suite reports PASS.
- Next release: tag the chosen version on clean `main`, let the Windows
  workflow validate and package that tag, then create a reviewed draft from the
  device. No validation gates are bypassed.
- Initial proof `39133903189` was cancelled after local tests exposed an
  annotated-tag selection edge case; the corrected run passed.
- Release scope: create validated ZIP artifacts; release notes and draft
  creation remain explicit device-side steps. No public release is requested
  now.

## October 18: evidence before the proving run

The latest failed job passed dependency setup and ran 167 of 175 jobs before
stopping. `tools/flat_draw_pixels.py --verify-fixture` reported that the motion
fixture lacked two consecutive WARP frames. Its source and the C++ fixture
writer are unchanged between that run and the starting source `043d7ff6`.

The failed run uploaded only its build log and timing diagnostics, so its frame
manifests cannot establish whether frames were missing or mixed with another
capture. The next workflow retains that fixture instead of losing the evidence.

Earlier main-branch run `38979120037`, source
`2dd897b1a2279ebe35056ee247975cb5c7825743`, stopped in a source-contract
assertion in `sharp_weapons_test`: the INI description was expected to mention
the shield strike ripple. The later run reached the different
fixture-verification failure.

The historical release workflow is on another branch and uses obsolete build
options and installer assumptions. The temporary Windows installer test
workflow was removed from main by `519ec0bf`. A current workflow must use the
existing full-build receipt, one installer, and all three native release
archives.

## October 18: capture directory collision and local check

The old production `FlatDrawCapture::arm` gave two independent instances the
same `_1` suffix. A no-GPU, no-file-write harness using the real header found
`COLLISION pair=0` on this device. The fixed code obtains a process-wide atomic
capture ID; the same harness found no collision in 10,000 adjacent pairs. The
mono-resolve rig also now fixes the clock to one timestamp while arming two
independent instances and asserts different directory names without writing
files. It remains in the shard that exercises the GPU capture.

The full `flat_mono_resolve_test.exe` rig compiled with local MSVC 14.44
through `tools/compile_cache.py`; `tools/flat_draw_pixels.py --self-test`
passed and now prints expected/found frame IDs and the directory if the motion
fixture is mixed. Wine could not create the WARP device (`FAIL: WARP device`),
so the native Windows full build is still the deciding gate.

## October 18: workflow tag selection

A branch proof originally always made a lightweight runner-local CI tag. On a
commit already carrying an annotated release tag, Git still selected the
annotated tag, so the expected stamp did not match. A scratch Git repository
reproduced that failure. The workflow now reuses the exact supported version
tag at HEAD, or creates the runner-local proof tag when HEAD is untagged.

The actual PowerShell block passed scratch checks for real release tags,
annotated and lightweight tags on branch proofs, an untagged branch proof, and
a clear rejection of an unsupported exact tag. Actionlint and PowerShell syntax
checks passed before restarting the native proof.

## October 18: native proof completed

[Run
39134563542](https://github.com/characterecho-sean/edvr-unofficial-patch/actions/runs/39134563542)
passed at source `8d5d25c3263705fcc5b7d98396be971129f51cf7`. The complete
`build.bat --wait-lock --jobs 4` took 19 minutes 37 seconds, including the
production DLLs, test rigs, installer and final gates. The log ends with
`Native OpenXR build and all gates passed.`

The full-build receipt has status `full-pass`, describe string
`v0.0.0-ci.39134563542.1`, and input fingerprint
`1075b69559f4fb6662fe840f5c5c6c7b85b0e49d7aa69023dd5ad5a769751b70`.
`package.bat` then produced the main, Windows-installer and Linux-installer
ZIPs. All three downloaded archives passed `sha256sum -c SHA256SUMS` on this
device; the Linux scripts retain executable ZIP attributes.

Logs, receipt and proof packages are retained locally under the Git-ignored
`.toolchains/windows/ci-39134563542/`. The proof tag exists only on the runner;
no release version tag or public release was created.

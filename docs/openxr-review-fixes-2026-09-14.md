# OpenXR branch review corrections

Astra reviewed branch `9422eef` against main, with parent validation of its
findings and a separate packaging audit. Three P2 findings were accepted. This
checkpoint records their corrections; it does not qualify additional headset
behavior.

## Runtime failure notification

A composition failure could permanently retire the frame boundary while leaving
session polling healthy. Subsequent frames failed without notifying Elite. The
loading path could also stop its service without delivering a quit event.

Fatal frame or loading failures now invalidate published poses, rendering
availability, cached treatments and timing, close admission to further native
operations, and queue one OpenVR quit event. Event polling remains available so
Elite can receive that event and shut down. Ordinary texture rejection remains
recoverable. Normal session STOPPING and READY restart behavior are preserved.

Regression tests inject negative composition errors, positive timeout results
and loading failures through the host, then check that no further frame work
occurs and quit is delivered only once. These are fake-runtime checks and
require no headset.

## GUI and CLI verification

The GUI installer writes CRLF startup configuration; the CLI writes LF. The CLI
verifier now accepts those equivalent line endings while retaining exact
comparisons for configuration values and DLL hashes. Receipt recovery still
uses original byte hashes. Its regression checks cover GUI-style configuration,
changed configuration rejection and verification without file or timestamp
changes.

## Consistent optional DLSS packaging

`package.bat <version> --no-dlss` retains its original allow-missing meaning:
it permits a build made without the DLSS SDK. A build containing DLSS keeps the
matching loose DLL and NVIDIA notice alongside the installer that embeds that
runtime. Packaging refuses mismatched or absent components, including an
embedded runtime with no matching release file or notice.

The packaging self-test now exercises resource-content validation rather than
stubbing the whole validator. The full build also checks the actual linked
installer's resources against the release payload after linking. This gate
reads the executable as data and does not run the installer.

## Validation

The absolute-path full build passed with all 541 source hashes unchanged during
compilation and testing. The native host suite passed 162 checks, including the
new fatal-failure regressions and seeded publication invalidation. The frame
suite passed 154 checks. Installer and packaging self-tests passed, including
CRLF verification without writes and embedded/loose DLSS mismatches. All 255
configuration keys passed the contract check, and the linked installer's actual
resources matched the release payload.

Astra re-reviewed the runtime and deployment corrections; the parent also
traced admission, quit delivery and teardown. The requested non-boundary
loading-failure coverage was included before building. Build evidence is
retained locally in `build/openxr-review-fixes-build.log` and
`build/openxr-review-fixes-build-sources.json`. The compiled version is
`v0.16.2-98-g9422eef-dirty`.

The private `--no-dlss` review archive at `dist/edvr-0.16.2-review-fixes.zip`
was produced from that full build. All eleven archive entries match their
source payload, including the DLSS runtime embedded in the installer and its
notice. Archive SHA-256:
`5f2efe11395acb60864689732ee0efb1ab332bbb38525b1d83b43a04be6c4d19`.

No Frontier installation or headset flight is part of this review correction.
The consolidated feature checklist remains in
`openxr-feature-parity-2026-09-14.md`; performance comparisons and experimental
features remain deferred. The broader real-shader build-gate gaps noted in the
review are separate from these three corrections.

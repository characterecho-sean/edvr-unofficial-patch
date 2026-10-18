# Building EDVR

How to build EDVR from source, check the build without the game or a headset,
and what the optional DLSS mode and the release package need.

Building needs Visual Studio 2022 with the C++ workload, and Python with the
numpy and Pillow packages, which several of the tools the build runs import:

```
pip install -r tools\requirements.txt
```

For a release-capable build, fetch all four pinned dependencies. The fetch
tools verify their sources and staged files; the build verifies them again:

```
python tools\fetch_ngx.py
python tools\fetch_ffx_dx11.py
python tools\fetch_xess.py
python tools\fetch_openxr_loader.py
```

Then build:

```
build.bat --wait-lock --jobs 4
```

That produces the native graphics/runtime pair, `build\openxr_loader.dll`, and
`build\smoke.exe`; the last of these checks the build without the game or a
headset:

```
build\smoke.exe build\d3d11.dll
```

A full build leaves your desktop alone, so you can keep typing in another
window while it runs. The runner holds every test exe to that and fails the
build if one shows a window or takes the keyboard focus; see
`docs\build-focus-steal-2026-09-29.md`.

For a native Windows build from Linux, use the **Native Windows release build**
workflow (`.github/workflows/release.yml`). It runs on Windows Server 2022 with
Visual Studio 2022, Python 3.12, and all four pinned dependencies. It invokes
the full `build.bat --wait-lock --jobs 4`, including every test rig and the
installer gate, then calls `package.bat`. The packages contain the exact
version stamp checked by `package_native.py`. Each successful run uploads the
full build receipt, installer, native DLLs, three release ZIPs, and
`SHA256SUMS`. Build logs, phase timings, and failed flat pixel fixtures are
uploaded separately even when a gate fails. The workflow does not create a
GitHub release or use signing secrets.

A push to a `codex/release-*` branch runs the entire build and package path. If
the commit already has a release tag, the runner uses its exact version;
otherwise it creates a temporary local tag such as `v0.0.0-ci.123456789.1`
before building. This makes the version stamped into the DLLs and installer
match the package version. The temporary tag exists only on the runner; either
branch result is a proof artifact, not a release asset. The branch push trigger
works before this workflow reaches `main`. GitHub does not offer its manual
**Run workflow** control for a new workflow until the workflow file is on the
default branch.

After the workflow is on `main`, a manual branch run is also possible:

```powershell
gh workflow run release.yml --ref main
```

These examples use PowerShell syntax compatible with Windows PowerShell 5.1. On
Linux, run them in `pwsh` with GitHub CLI installed. Inspect a proof run and
download its packages. Use the run ID shown by `gh run list`; a rerun
increments `run_attempt`:

```powershell
gh run list --workflow release.yml --branch codex/release-workflow --limit 5
$runId = 'RUN_ID'
gh run view $runId --log-failed
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

If a run fails, download `edvr-diagnostics-$runId-$runAttempt` in the same way
and read its build logs and saved fixture before changing a gate. The complete
Actions job log is also available through `gh run view $runId --log`.

For a real release, merge and push the validated source to `main`, then make
and push its exact version tag from a clean checkout. A pushed `v*` tag runs
the same full validation and packaging at that tag; the workflow refuses a tag
whose name is not a semantic version:

```powershell
git switch main
if ($LASTEXITCODE -ne 0) { throw 'Cannot switch to main' }
git pull --ff-only
if ($LASTEXITCODE -ne 0) { throw 'Cannot update main' }
if (git status --porcelain) { throw 'Checkout must be clean before tagging' }
$version = 'VERSION'            # for example '0.20.0'
git tag "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot create version tag' }
git push origin "v$version"
if ($LASTEXITCODE -ne 0) { throw 'Cannot push version tag' }
gh run list --workflow release.yml --limit 10
$runId = 'TAG_RUN_ID'
gh run watch $runId --exit-status
if ($LASTEXITCODE -ne 0) { throw 'Tagged Windows build failed' }
$repoName = gh repo view --json nameWithOwner --jq .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve repository name' }
$runAttempt = gh api "repos/$repoName/actions/runs/$runId" --jq .run_attempt
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve run attempt' }
New-Item -ItemType Directory -Force -Path "edvr-artifacts/$runId" | Out-Null
gh run download $runId -n "edvr-packages-$runId-$runAttempt" -D "edvr-artifacts/$runId"
if ($LASTEXITCODE -ne 0) { throw 'Package download failed' }
Push-Location "edvr-artifacts/$runId"
try {
    sha256sum -c SHA256SUMS
    if ($LASTEXITCODE -ne 0) { throw 'Package checksum verification failed' }
} finally { Pop-Location }
```

Write and review release notes before creating the draft. Generate the Thanks
section from the previous tag, then write each contributor's sentence by hand.
Only merged PR authors are named; issue and log reporters get the tool's
general line. Review the final notes file, then attach the three verified ZIPs
and `SHA256SUMS` to a draft release:

```powershell
python tools/release_credits.py --from vPREVIOUS --to "v$version" --thanks --out thanks.md
if ($LASTEXITCODE -ne 0) { throw 'Review release credits before continuing' }
# Edit release-notes.md and include the reviewed ## Thanks section from thanks.md.
$assets = @(
    "edvr-artifacts/$runId/edvr-$version.zip"
    "edvr-artifacts/$runId/edvr-installer-$version.zip"
    "edvr-artifacts/$runId/edvr-installer-linux-$version.zip"
    "edvr-artifacts/$runId/SHA256SUMS"
)
gh release create "v$version" @assets --draft --verify-tag --notes-file release-notes.md
if ($LASTEXITCODE -ne 0) { throw 'Draft release creation failed' }
```

For a prerelease tag, add `--prerelease` to `gh release create` as well.

The draft is the review point before publication. A failed build has no package
artifact and should never be used for a release.

The optional DLSS mode, and an installer that carries NVIDIA's runtime, need
the DLSS SDK on the machine. Its licence keeps it out of this repository, and
it is not in the graphics driver either. This command

```
python tools\fetch_ngx.py
```

fetches one pinned commit of NVIDIA's public SDK repository into
`%LOCALAPPDATA%\EDVR\ngx-sdk` and checks the runtime's hash against the pin in
the script. `EDVR_NGX_SDK` points the build at a copy somewhere else.

Generating the installer's resources needs the native pair and the bundled
loader; there is no legacy OpenVR-only package. `package.bat <version>
--no-dlss` packages a build made without the DLSS SDK. When the build contains
DLSS, the archive keeps its matching DLL and NVIDIA license notice next to the
installer that embeds it.

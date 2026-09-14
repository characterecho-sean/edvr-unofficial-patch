# Native-only release migration

Sean requested removal of backend fallback logic so every EDVR user moves to
native OpenXR. Normal CLI installs, GUI installer resources and release ZIPs
now require the native graphics/runtime pair and bundled Khronos OpenXR loader.
Windows chooses the OpenXR runtime; the release does not depend on finding a
loader inside SteamVR or another headset vendor's installation.

## Behavior

- The standard `d3d11.dll` and `openvr_api.dll` build outputs are native.
  Historical proxy artifacts are retained only as regression fixtures and are
  excluded from release packaging.
- The GUI installer preserves graphics-mod chaining, settings and the game's
  original VR DLL for uninstall. It never deploys a partial native package. A
  game update's newer original replaces the recovery original only after both
  versions have been backed up.
- Normal `install_edvr.py` invocations always install the complete native
  package. `--dll` and `--openvr` remain compatibility aliases for that same
  package. A fresh install does not require an original OpenVR DLL to run.
  Existing foreign graphics DLLs require the GUI installer's chaining logic.
- Native startup refuses an identified Elite process if its audited LibOVR
  routing hook cannot be established. The installer checks the supported
  executable hash before writing. Unknown game revisions require an updated
  profile; they do not enable a legacy backend.
- A manual package in `Openvr\win64` works without `edvr_openxr.ini`: the
  runtime uses its sibling loader, the game's native graphics DLL and Windows
  runtime selection. A present invalid configuration fails explicitly.
- The Khronos loader is pinned to official SDK release 1.1.46. Archive, DLL and
  notice hashes are verified before build/install/package. See [dependency
  provenance](../third_party/openxr/PROVENANCE.md).

## Review corrections

The initial drafts would have weakened graphics-mod chaining and installer
recovery. Review preserved the existing graphics-chain planner and INI merge,
added snapshots for replaced/deleted files, and required verified rollback
before reporting restoration. Explicit uninstall/receipt recovery remains a
file recovery operation; it is not automatic VR backend fallback.

Embedded payload validation now follows PE export ordinals, requires the exact
native runtime ABI, checks all native graphics providers, and checks the
read-only startup marker's four fields. A legacy graphics DLL cannot satisfy
the native package contract. The native runtime is identified as EDVR rather
than mistaken for an original OpenVR DLL.

The CLI's old default deployment branch was removed. Version 2 receipts cover
all installed components, allow files that were absent before installation, and
restore bytes or absence as appropriate. Version 1 receipts remain usable for
earlier deliberate recovery. Tests inject receipt update failures and check
that the installed package remains recoverable.

## Validation

The full absolute-path `build.bat` run passed with 541 source hashes unchanged
during compilation and testing. The native module's local/package startup tests
passed 28 checks, and the installer suite passed, including actual built PEs,
marker/provider mutations, graphics chaining, INI merge, original recovery and
transactional rollback. The config contract passed all 255 keys.

The final CLI-only receipt ownership correction was made after that build and
passed its complete self-test again, including concurrent receipt creation and
corrupted rollback injections. No C++ source changed after the successful full
build. Its final tool hash is recorded separately in the qualification file.

Release packaging validated the native exports, pinned loader/notices and all
embedded installer resources against the source payload. A private review ZIP
and standalone installer were produced. The GUI installer preview against
Frontier passed without writing. The sanctioned CLI then installed the native
package and verified every DLL and startup configuration. `edvr.ini` and
`openvr_api_orig.dll` retained their exact pre-install hashes.

Installed version: `v0.16.2-97-g83dda1b-dirty`.

| Artifact | SHA-256 |
|---|---|
| Native graphics | `138c2e7d3e8c677d6da4a5780984b424751faea45e8a407481fa03e49a5b0dbf` |
| Native runtime | `63234557fe1b4c048b81eba18753139ea8eebc1c5ba36ebf00453e01fe171e9b` |
| Khronos loader | `a231a20944153cfda9551af135a3e58519f77007f28afc76d3c5d23763be8bde` |
| Local startup config | `5ae1485d89626db25de0a4962cb34d37856d62f4f3feca717b9b033a102a38cb` |

The private archive is `build/openxr-native-only-20260914`, including the
paired binaries, previews, verification output, build source hashes and
qualification record. No new headset flight has been claimed for this
migration. The preceding successful Air Link menu/recenter/exit flight remains
documented in the [retest checkpoint](openxr-airlink-menu-exit-2026-09-14.md).

## Next flight

Launch Frontier normally with the desired OpenXR runtime selected in Windows.
Check that EDVR and the main menu appear in both eyes, recenter still works, F8
shows the native runtime and metrics, and quitting closes the process. With
Meta or VDXR selected, SteamVR should not be needed. The broader combined
feature checklist remains in the [parity
checkpoint](openxr-feature-parity-2026-09-14.md); experimental features and
performance comparisons remain deferred.

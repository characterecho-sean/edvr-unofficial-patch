# Native Frontier manual launch and menu checkpoint

This checkpoint follows the first native Frontier flight at `642907b`. That
flight rendered normally through PiOpenXR, but F8 could not open because the
native compositor had no EDVR menu consumer. The graphics proxy was loaded;
reinstalling the same files could not fix the missing callback path.

## Scope

The native DLL now reads `edvr_openxr.ini` beside itself when no explicit API
configuration or bootstrap environment is present. The installer writes the
absolute loader, installed graphics proxy and runtime manifest paths, plus
version 1 and separate-device selection. Malformed or partial environment
configuration fails instead of falling through. Discovery probes do not
configure the module or change the environment. Local runtime selection uses
only the game's process environment; an inherited valid `XR_RUNTIME_JSON` wins,
and a locally applied value is restored after cleanup without overwriting
another component's change.

The new private, versioned menu provider runs on the existing game graphics
callback. The native host publishes the frame's head pose, full eye poses and
asymmetric frusta before menu composition. The provider composites into a
game-device texture; the host retains that output through the existing shared
capture copy to its separate OpenXR device. `WaitGetPoses` retains its existing
scheduling. Shutdown closes the menu through a CPU-only callback and keeps the
qualified separate-device drain order.

Menu capability is independent of the legacy glitch consumer. Reference-space
changes and invalid tracking release keyboard capture and retire the visible
menu. The graphics runtime detector recognizes the native DLL's actual private
exports and reports the pending submission effects instead of advising a
reinstall. Temporal AA, crop/resolve/sharpen ordering and the other legacy
compositor effects remain unported; menu visibility is not evidence of their
operation.

Native Scene Init opens an adjacent `edvr_openxr_YYYYMMDD_HHMMSS_mmm_PID.log`
using UTC. It includes the build version, configuration source, runtime/device
identity, first menu captures for each eye, pose failures, frame/menu totals,
GPU teardown stages and callback retirement. Existing stdout diagnostics remain
available to desktop fixtures. `tools/edvr_log.py --file <path>` recognizes the
native build record.

## Verification

The absolute-path full build passed with all 485 source hashes unchanged. The
menu fixture passed 40 checks using real WARP rasterization, both-eye pixel
readback, graphics-state restoration, the versioned client, exact copies onto a
separate consumer device and CPU retirement. The four existing module modes
passed 239/245/239/245 checks; the new local bootstrap mode passed 14. Shared
transport passed 261, shutdown 63, and the config contract passed all 254 keys.
Native module tests produced durable version and shutdown records without an
OpenXR runtime. The installer and log-reader self-tests also passed. No new
headset or in-game result is implied by these desktop fixtures.

The fixed package is `build/openxr-native-menu-20260913/`. Its build is
`v0.16.2-77-gab80a6c-dirty`, graphics stamp `6AA7379C`. The package includes
the full build log, source/binary hashes, a real desktop native trace, install
preview, installation output and verification output. At 2026-09-13 23:59:25
UTC the sanctioned installer directly installed and verified the following
Frontier files:

| Installed file | SHA-256 |
|---|---|
| `Openvr/win64/openvr_api.dll` | `fcb5c344f460a3955ba66317ffedbb4815f41d90dfb6133c08f931b1054722e4` |
| `d3d11.dll` | `d0fce06d3ef73db05db423c023698e839b859827ce0b6c9fa19b61cacc6afa71` |
| `Openvr/win64/edvr_openxr.ini` | `e061776e5999a9f067332ddced760b009af7bb4f930ea5c47d13abc8c61ecbcc` |

The game was closed for installation. The original OpenVR DLL (`243a818d...`)
and live game INI (`22f91f84...`) retained their hashes. No launcher was
started. Manual Frontier qualification remains pending.

## Manual Frontier flight

Use the Frontier installation directly under the user's authorization. Install
and verify the native and graphics DLLs together with `tools/install_edvr.py
--native-openxr --dll --no-backup --native-loader <absolute-loader>
--native-runtime <absolute-manifest> --target frontier`. This writes only the
paired DLLs and `Openvr/win64/edvr_openxr.ini`; the live game INI and original
OpenVR DLL remain outside the payload. Preview uses `--dry-run`; exact
verification uses `--verify-only`. No receipt, backup, scripted launcher or
automatic restoration is required for this mode.

Restart the Frontier launcher to discard the previous scripted launch's
inherited environment, keep SteamVR closed and launch normally with the Pimax
headset through PiOpenXR. Verify normal startup, both-eye rendering and
tracking, F8 visibility/readability and menu anchoring during head movement.
Close the menu and exit the game normally. Collect the matching graphics and
native logs, confirm `source=local`, nonzero menu captures for both eyes and
completed native shutdown with callback retirement. Treat submission effects
that remain pending separately from bootstrap, menu and transport failures.

# OpenVR export census fixture

`build.bat` builds `openvr_export_census_test.exe`, the real proxy and three
uniquely named fake runtimes: `export_census_fakevr.dll`,
`export_census_missing.dll` and `export_census_reentry.dll`.

Run `build\openvr_export_census_test.exe --self-test build`. Add `--dry-run` to
validate inputs and describe staging without writing files. Each enabled,
disabled, missing-export and loader-reentry case runs in a fresh child beside
an isolated INI, proxy and renamed fake runtime. Each child has a 20-second
timeout and suppresses interactive crash dialogs. Artifacts stay under the
build directory for failure inspection.

The fixtures verify forwarding results, exact invocation counts, null error
arguments, missing-export stubs, suppression, diagnostic read faults, paired
lifecycle records and bounded concurrent polling. The enabled case must produce
records for all five exports, so an unwrapped proxy cannot silently pass. The
test never loads an installed OpenVR runtime or edits a game installation.

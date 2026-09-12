# Frontier LiveCopy integration flight

Status: installed, not yet flown. Sean confirmed Pimax through SteamVR for this
test. Main's PR #33 was merged into `codex/openxr-port` at `06636b2`; the full
paired build and its regression gates passed. The startup shader bytecode and
menu-worker exit fixes remain in this branch. No OpenXR transport or new game
GPU timer is enabled.

## Installation and environment

Use the Frontier Odyssey installation. Preserve its tuned INI. Its current
settings include exposure sharing, black void, the F8 menu and
`advanced.openvr_census=on`. No active context-hook override or diagnostic
probe setting was found; confirm the resulting mode in the flight log.

Build from the final clean commit, then install and verify both proxies:

```powershell
python tools/install_edvr.py --target frontier --dll --openvr --dry-run
python tools/install_edvr.py --target frontier --dll --openvr
python tools/install_edvr.py --target frontier --dll --openvr --verify-only
```

Record that commit as the flight's expected build. Record actual per-eye sizes,
runtime, GPU and DLSS version from its logs; a headset name alone does not
establish those values. Keep the existing settings for this run. Specify both
payload flags: the preview with `--openvr` alone selected only the VR DLL.

The clean `f08098c` full build passed in
`build/frontier-livecopy-clean-build.log`. Both DLLs were installed and
independently verified on 2026-09-12. Their SHA-256 hashes are:

```text
d3d11.dll      51670D15C481DBEF409617371202A03CE906C0FEAACFDE60EB2AD489BB61C7D9
openvr_api.dll CDD8A8F6EF454005257135467363BDC3FCC50EE407AC918191E42647091FB41A
```

The INI hash was identical before and after installation:
`A32D631834E5C1EEEFAA03367750A7A4211652869EB4976900DC11CF530EC1A9`. The
subsequent documentation commit does not change this flight's binary version.

## Flight steps

1. Start Frontier Elite with Pimax through SteamVR. Check that the intro starts
   promptly and the previous long black screen has not returned.
2. Enter gameplay and play for 5–10 minutes. Move and turn your head, inspect
   cockpit text and lighting, and check that both eyes render and track
   normally. Report any freeze, crash, flicker or new stutter.
3. Open and close EDVR's menu with F8. If on-foot play is available, inspect
   the on-foot screen's surround and distance to exercise the context-hook
   fixes. Record any skipped scene so its feature checks remain untested.
4. Exit the game normally and confirm it closes without a crash or hang.

## Evidence to review after the flight

Check the graphics and VR logs separately against the installed commit before
reading counters. This flight's expected build is `f08098c`, even if branch
HEAD has advanced since installation:

```powershell
python tools/edvr_log.py --target frontier --tag gfx --expect-build f08098c --version
python tools/edvr_log.py --target frontier --tag vr --expect-build f08098c --version
python tools/edvr_log.py --target frontier --tag gfx --expect-build f08098c --grep 'context hook|exposure fix installed|vScreen fixes installed|vScreen totals|hook|disabled|sentinel|PROBE|exception'
python tools/edvr_log.py --target frontier --tag gfx --expect-build f08098c --grep 'census|first Present|shader|shutdown'
python tools/edvr_log.py --target frontier --tag vr --expect-build f08098c --grep 'census|shutdown'
```

PR #33 passes this configuration's flight gate only when the log confirms live
private tables for both exposure and vScreen, with active eye-draw and relevant
fix counters during gameplay, alongside Sean's normal rendering report. A
process that survives with hooks disabled, a sentinel recovery path or a
probe-only configuration does not pass. Read the periodic hook-retention
diagnostics and any starvation warnings as well as installation messages. Hook
retention is logged when degraded; absence of that warning alone is not proof
that the hooks ran. Eye-draw counts vary by scene; do not require another
flight's fixed count.

Recheck the bounded VR-phase ordering across pose waits, both distinct eye
submissions and Present. Compare caller threads, immediate contexts and eye
texture devices with the published owner. Keep startup samples separate and
respect each event budget: exhausted budgets cannot prove missing calls. Work
after second-eye Submit or Present remains outside the proposed
render-to-submit span, as recorded in the earlier census flight.

Retain the actual log names, both version stamps, relevant counters and user
observations here after review. This run checks merged hook behavior and
startup/exit regressions; dispatch overhead and GPU timing accuracy still need
their later matched measurements. The native shared-query test is desk evidence
only.

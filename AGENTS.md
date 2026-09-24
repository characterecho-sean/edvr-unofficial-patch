# EDVR — working notes for coding agents

This is the only instructions file in the repo. CLAUDE.md is a one-line @AGENTS.md import because Claude Code reads only CLAUDE.md; do not add content there.

Unofficial VR fixes for Elite Dangerous: Odyssey. A proxy DLL
(`d3d11.dll` beside the game) and EDVR's own OpenXR runtime for
Elite (`openvr_api.dll` in `Openvr\win64`), a config file, and an
installer that carries all three.

The expensive resource on this project is not tokens, it is **test
flights**. Every wrong hypothesis costs a build, an install, a headset
session and a log. Everything below exists to spend fewer of them.

## Shell

Use the **PowerShell tool**, not Bash. Windows PowerShell 5.1: no `&&` or
`||` (use `;` and `if ($?)`), no ternary, no `??`. Don't redirect a native
exe's stderr with `2>&1` — 5.1 wraps each line in an ErrorRecord and sets
`$?` false on a clean exit.

`build.bat` must be launched **by absolute path**; a `cmd` invocation that
never ran the batch still exits 0, which has read as a successful build.

## Reading at low token cost

Every tool call re-sends the whole context, so anything read into it is
paid for again on every later call. Measured 2026-09-15: a 359-call
session averaged 427k tokens per call and never compacted.

- **Never read a large file whole.** `edvr.ini` (148 KB), `build.bat`
  (94 KB), `README.md`, `vscreen.cpp`, `temporal_pass.cpp` and most docs
  are far past the 30 KB line. Grep for the key or symbol, then read a
  range with the Read tool's offset and limit. A whole-file dump through
  PowerShell is not a way to read a file.
- **An investigation doc starts with a `## Status` block** of at most 60
  lines: state, open hypotheses, ruled-out pointer, next flight. Read the
  block first and go into the journal only where it points. Update the
  block whenever the doc changes; add one to any doc that lacks it.
- **Keep new docs small.** Append a dated entry to the arc's own doc
  rather than starting another review doc; a review that must be its own
  file stays under about 15 KB.
- **Delegate to subagents on the cheapest model that will do the job
  well.** Research, exploration and implementation go to a subagent, and
  only its conclusion comes back, so the main context stays small. Pick
  the tier by the task: mechanical, fully specified work with a check at
  the end (a reflow, an install and verify, a log read, a rename) goes to
  the cheapest tier; work needing judgment across files or a build gate
  goes to a mid tier; the top tier stays in the main context for diagnosis
  and decisions, where a wrong call costs a test flight. If a cheaper tier
  fails its check or reports doubt, rerun on the next tier up with its
  report attached rather than finishing by hand in the main context. Do
  not delegate a job of two or three tool calls: a subagent's own overhead
  exceeds it.
- Memory topic files point at the doc; they never mirror its journal.

## The sanctioned tools

These four operations go through `tools\`, and **only** through `tools\`.
Each was an ad-hoc one-liner retyped once a session for a month; the
evidence is the dozens of backups in both game directories named six
different ways, and a live `edvr.ini` a stray PowerShell anchor once
corrupted. Do not regenerate the one-liner — extend the script.

| Operation | Use | Never |
|---|---|---|
| Install a build next to the game | `python tools\install_edvr.py` | `Copy-Item` onto the game directory |
| Check the install matches the build | `python tools\install_edvr.py --verify-only` | eyeballing timestamps |
| Find and read a flight log | `python tools\edvr_log.py` (census aggregate: add `--tally vh [--frame N]`) | `Get-Content -Tail` + `Select-String` |
| Reflow release notes or docs | `python tools\reflow_notes.py` | reflowing by hand or by regex |

```bash
python tools\install_edvr.py --target steam --dry-run
python tools\install_edvr.py --target steam
python tools\edvr_log.py --target steam --expect-build HEAD --version
```

Anything that writes files takes `--dry-run`, and `--dry-run` **writes
nothing at all** — no copies, no backups, no directories. This project has
already shipped a `--dry-run` that wrote files; the self-tests assert the
property directly.

Every tool in `tools\` carries `--self-test`, and `build.bat` gates on it.
A new tool gets one too, and gets added to that gate — a parser that
drifts from the format it reads must fail in the build, not in the ten
minutes after a flight finally reproduced the effect being chased.

## Diagnosis discipline

- **Never ship a fix on an untested hypothesis.** State the hypothesis,
  name the specific log line, draw call, counter or struct field that
  would confirm or refute it, and get that evidence before editing code.
- **The first question about any flight log is whether it is the right
  build.** `python tools\edvr_log.py --target steam --expect-build HEAD`
  exits 2 when it is not. A log from a stale DLL is not evidence, and
  counters read off one have cost a session before.
- **When a flight refutes a hypothesis, write it down** as a one-line
  `ruled out: X, because Y` in the investigation doc under `docs\`, before
  proposing the next one. These arcs run for days across separate
  sessions; an idea that is not recorded as dead gets re-proposed.
- **Prefer root causes to compensation.** Do not propose sharpening,
  contrast tweaks, dead bands or threshold nudges to mask a geometry,
  motion-vector or timing bug. Sharpening over DLSS blur was proposed once
  and rightly rejected: the motion vectors were wrong.
- **Enumerate before you build.** When several causes are plausible, list
  them with a discriminating log signature for each and instrument them
  together, so one flight can eliminate several. One flight per hypothesis
  is the most expensive way to run this project.
- **State the environment a rendering fix depends on**: VR runtime
  (OpenVR / Oculus LibOVR / OpenXR), headset and per-eye render size,
  DLSS version, and any fixed-size internal table. A supporter's Elite
  using the Oculus native SDK meant four OpenVR-side fixes were inert on
  their rig and nothing said so.

## Build and verify before commit

- **Every C++ edit compiles before you report it as done.** Run the full
  `build.bat` by absolute path and read the tail of its output.
- The normal `build.bat` path is the validation build: it runs the Python
  self-tests, compiles the production DLLs, runs the test rigs, builds and
  checks the self-contained installer, and writes
  `build\full_build_receipt.json` only after all gates pass. Green means
  green; do not report success off a build you did not read.
- After that full build, commit the same source tree. For the install-only
  rebuild that gives the DLLs the clean commit's version, use
  `build.bat --dll-only` by absolute path. It requires a clean working tree
  and a matching full-build receipt, rebuilds and validates the production
  DLLs, and skips the test rigs and self-contained installer. It is a
  promotion step, never a substitute for validating changed source; if its
  receipt check fails, run the full build again.
- After editing, re-read the changed region for the things that have bitten
  here: declaration order, a duplicated census or log string, a shader
  entry-point name collision that would silently disable a new instrument.
- **Trace a new instrument end to end before flying it.** A summary path
  that set its finished flag before calling the report was dead on
  arrival; a fix that returned early shadowed the probe meant to measure
  it, and cost a flight. Ask what would appear in the log if the new code
  never ran, and make sure that is distinguishable from success.

## Editing config and INI files

- **Never edit a live `edvr.ini` with a regex or a PowerShell anchor.**
  Read it, edit it with the Edit tool, diff before and after. A bad anchor
  has corrupted Sean's working config.
- `edvr.ini` in a game directory carries the settings of whoever flew
  last. A reinstall does not undo an edit to it. `install_edvr.py` does not
  touch it unless `--ini` says so, and backs it up when it does.
- Settings mirror to `%LOCALAPPDATA%\EDVR\<leaf>-<store>\` and are restored
  from there, because a game update once wiped the whole install directory
  including the ini.
- **Config values name the functionality, never the mechanism.** A key
  says what the user gets — `on`, `off`, `auto` — not the name of the
  technique inside.

## Scope control

- **Do not remove or rename a feature or config key without quoting the
  exact key and its current behaviour and asking first.** Similar names
  are easy to confuse: three separate things here share the word
  "witchspace", and only `fix.witchspace_stars` touches explosions. A
  removal was asked for, carried out against the wrong one, and reverted
  in the same turn.
- Do the work asked for. If something adjacent is worth fixing, say so;
  don't fold it in.

## Git

- Solo project: **branch, commit, merge to main, push.** No pull requests.
- **The task is not done until it is pushed.** After merging run
  `git push`, then `git log origin/main -1` and confirm the commit is
  actually there. A commit made and never pushed has been noticed from the
  other side more than once.
- **Write commit messages to a file and use `git commit -F`.** PowerShell
  5.1 does not escape embedded quotes when building a native command line,
  so a message with `"` in it splits into stray pathspecs. Write the file
  as UTF-8 without a BOM.
- Reading a file back to verify it: decode explicitly with
  `[Text.Encoding]::UTF8.GetString([IO.File]::ReadAllBytes(path))`.
  `Get-Content` on a BOM-less UTF-8 file decodes as the ANSI codepage and
  turns every em-dash into mojibake — that reached a public GitHub comment
  once.
- Worktrees under `.claude\worktrees\` and `.Codex\worktrees\` share one
  stash stack with the main checkout. Never use bare `git stash` / `git
  stash pop`.

## Layout

| Path | What |
|---|---|
| `src\d3d11\` | the graphics half: hooks, shader fixes, the draw census |
| `src\openxr\` | the native OpenXR runtime: the OpenVR ABI Elite calls, translated to OpenXR |
| `src\openvr\compat\` | the OpenVR ABI header |
| `src\installer\` | the self-contained installer and its log bundler |
| `src\common\` | config, logging, the crash sentinel |
| `tools\` | Python tools, each with `--self-test`; C++ test rigs in subdirs |
| `docs\` | one investigation doc per arc — read its `## Status` block first |
| `build.bat` | full build, all gates; `--dll-only` is the receipt-guarded promotion path |
| `package.bat <version>` | builds, tests, then packages a release |

# EDVR — working notes for coding agents

This is the only instructions file in the repo. CLAUDE.md is a one-line @AGENTS.md import because Claude Code reads only CLAUDE.md; do not add content there.

These notes include the maintainer's workflow. In a fork or contributor's
clone, follow the project rules for tools, build validation, diagnosis and
config editing. Use your own shell and Git workflow, and submit changes by
pull request. The Git section, release-notes rules, and maintainer-only
merge, push-to-main, tag, release and workflow-dispatch steps elsewhere in
this file do not apply to contributors. The host-specific validation paths
still apply: native Windows uses the full local build, and other hosts use
the full Windows workflow for C++ changes.

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

These operations go through `tools\`, and **only** through `tools\`.
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
| Credit contributors in release notes | `python tools\release_credits.py --from <previous-tag> --thanks` | writing the credits from memory or ad-hoc `gh` queries |
| Make a worktree that can build | `python tools\worktree.py add <path>` | `git worktree add` alone (no deps) |
| Compile on Linux for a test flight | `python3 tools/linux_toolchain.py build-dlls` | running `build.bat` under Wine by hand |

Every release's notes carry a `## Thanks` section built from
`release_credits.py --thanks` and finished by hand. It names only the authors
of PRs merged in the release; everyone who opened an issue or sent logs gets
the tool's one general line and is not named. A person writes each sentence.
People known only by an anonymised label ("user 2", "the reporter") are not
named without the maintainer's consent.

```bash
python tools\install_edvr.py --target steam --dry-run
python tools\install_edvr.py --target steam
python tools\install_edvr.py --target steam --scope flat --xess
python tools\install_edvr.py --target steam --scope flat --verify-only
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

- **Choose the validation path by host.** On native Windows, every C++
  edit gets the full local validation build before commit: launch the
  full absolute path to `build.bat --wait-lock` and read the tail of its
  output. The GitHub workflow is optional there. From Linux or another
  non-native Windows host, use the GitHub Windows workflow below for
  full C++ validation. Wine and Proton do not count as native Windows.
- **Linux: a compile-only build.** `tools/linux_toolchain.py` runs
  `build.bat --compile-only` under Wine, with the same MSVC and Windows SDK as
  CI, and writes the production DLLs to `<root>/build/compile-build` for the
  main checkout or any worktree. It runs none of the gates, so it is not
  validation and not a release asset; CI stays the validation for a merge or
  release, and the full `build.bat` currently fails Wine's process/window gates
  (do not bypass them). Run it with host `python3` from any shell, no wrapper.
  Once per machine the user installs `wine msitools git` with sudo; then
  `python3 tools/linux_toolchain.py setup --dry-run`, and
  `setup --accept-license` only when the user has accepted VS 2022's Build
  Tools licence (it needs a Visual Studio licence). Microsoft's binaries, the
  Wine prefix and the SDKs sit in the Git-ignored
  `<main checkout>/.toolchains/windows/` and are never shared; `doctor` checks
  them.
- **Linux test flight (Steam Deck, Proton).** For a build Sean will fly, CI
  first is not required; CI is for a merge or release. Commit to the branch
  before compiling: a dirty tree compiles as `vX.Y.Z-dirty`, which
  `edvr_log.py --expect-build HEAD` will not match. Close Elite, then:

  ```bash
  python3 tools/linux_toolchain.py build-dlls
  python3 tools/install_edvr.py --target steam --scope flat --xess --compile-only --dry-run
  python3 tools/install_edvr.py --target steam --scope flat --xess --compile-only
  python3 tools/install_edvr.py --target steam --scope flat --xess --compile-only --verify-only
  python3 tools/edvr_log.py --target steam --expect-build HEAD --version
  ```

  To fly a worktree, give `build-dlls` and `install_edvr.py` `--root
  <worktree>`. `--target steam` finds the game on Linux
  (`tools/steam_library.py`: `~/.local/share/Steam`, `~/.steam/steam`, then
  every library in `libraryfolders.vdf`). `--compile-only` installs from
  `<root>/build/compile-build` instead of `build/`, and refuses without that
  build's `compile_only_receipt.json`, when an output no longer matches it, or
  when the tree has been edited or committed since (`tools/compile_only.py`
  holds the fingerprint, shared with the toolchain). It prints that the build
  is not Windows validated and writes `"build": "compile-only"` into the
  install receipt; `--verify-only` compares the game directory to that build
  and only warns about a moved tree. A real install runs under the host
  `python3`, which checks `/proc` for a running `EliteDangerous6*` (Proton); do
  not run it through the Wine Python, which cannot see Proton's processes and
  passes that check vacuously.
- For C++ changes on a non-native host, commit and push the candidate
  branch so Actions can validate that exact commit. The `release.yml`
  workflow is active on `main`; dispatch it with `gh workflow run
  release.yml --ref <branch>` (a push to `codex/release-*` already
  triggers the full build and package path, so do not dispatch a
  duplicate run). Find the branch run with `gh run list --workflow
  release.yml --branch <branch>`, verify its `headSha` matches the
  candidate with `gh run view <run-id> --json headSha`, then require `gh
  run watch <run-id> --exit-status` to pass before merging or releasing.
  This commit-before-CI order is necessary for remote validation; native
  Windows keeps the full-build-before-commit order above. Read the full
  job log and `edvr-diagnostics-<run-id>-<attempt>` on failure; on
  success inspect `edvr-build-<run-id>-<attempt>` and
  `edvr-packages-<run-id>-<attempt>`. Branch proof packages use a
  runner-local CI tag (or an exact existing tag) and are not release
  assets. See `docs/building.md` for the download, checksum and release
  commands.
- For an actual release from a non-native host, merge the validated
  source, push the exact clean `vX.Y.Z` tag, and require that tag's full
  Windows build and package run to pass. Download and verify all three
  ZIPs with `SHA256SUMS`, generate and review release notes including
  `release_credits.py`'s `## Thanks` section, then create a draft
  release; publication remains an explicit step. On native Windows, the
  full local build and `package.bat <version>` at the clean version tag
  are sufficient; GitHub Actions is not required.
- **One build at a time on this machine.** Run `build.bat --wait-lock` by
  absolute path: it waits for any other build to finish and then takes the
  lock and builds in one step. A bare `build.bat` that finds another build
  running prints "another build is already running" and exits 1. Do not run
  `build_lock.py --wait` and then `build.bat` as two commands; another build
  can take the lock between them.
- The normal `build.bat` path is the validation build: it runs the Python
  self-tests, compiles the production DLLs, runs the test rigs, builds and
  checks the self-contained installer, and writes
  `build\full_build_receipt.json` only after all gates pass. Green means
  green; do not report success off a build you did not read.
- Compiles go through `tools\compile_cache.py`, which skips a compile whose
  sources, project headers, flags, toolchain and `INCLUDE` are unchanged. Each
  invocation prints a `compile_cache:` summary line (compiled / reused, and why
  for the first misses); a hit does not replay the compiler's warnings.
  `build.bat --clean` or `EDVR_NO_COMPILE_CACHE=1` forces full recompiles.
  System and SDK headers are covered only by the `INCLUDE` path and the
  compiler's identity, not by content.
- On native Windows, after that full build, commit the same source tree.
  For the install-only rebuild that gives the DLLs the clean commit's
  version, use `build.bat --dll-only` by absolute path. It requires a
  clean working tree and a matching full-build receipt, rebuilds and
  validates the production DLLs, and skips the test rigs and
  self-contained installer. It is a promotion step, never a substitute
  for validating changed source; if its receipt check fails, run the
  full build again. The receipt fingerprints tracked files, untracked
  files git does not ignore, and the ignored dependency folders
  (`third_party\ngx`, `third_party\ffx-dx11`,
  `third_party\openxr\loader`, `third_party\xess`); `docs\`, `reviews\`
  and other ignored paths are not inputs, so writing a review or a doc
  does not stale it (`tools\build_receipt.py` states the rule and its
  self-test pins it).
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
  overwrite an existing one unless `--ini` says so (and backs it up when it
  does); a missing one it writes, from the newest copy in the `%LOCALAPPDATA%\EDVR`
  mirror when there is one, else from the shipped default.
- `edvr-flat.ini` is the flat profile's own settings file: while the flat
  profile is active, EDVR reads it first and falls back to `edvr.ini`.
  `--scope flat --ini` writes it (never `edvr.ini`), `--scope both --ini`
  writes both, and a scope that includes flat seeds it from `edvr.ini` when
  the game directory has none yet. Everything above about a live `edvr.ini`
  applies to it verbatim.
- Settings mirror to `%LOCALAPPDATA%\EDVR\<leaf>-<store>\` and are restored
  from there, because a game update once wiped the whole install directory
  including the ini.
- **Config values name the functionality, never the mechanism.** A key
  says what the user gets — `on`, `off`, `auto` — not the name of the
  technique inside.
- **Every key is one a user would set.** A key added to chase an effect
  (an A/B switch, an instrument switch, a threshold tuned by a flight) is
  temporary: list it in the arc's `## Status` block, and when the arc
  closes, ship the winning value as the behaviour and propose removing the
  key, quoted, as Scope control requires. A fix that always helps gets no
  toggle. The holo config sweep and the jitter and steady keys in the
  2026-10 cleanup build were both this debt, paid late.

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

## Agent material

Text addressed to agents rather than people stays where anyone can strip it
from a clone without reading it: `AGENTS.md` and `CLAUDE.md` at the root,
where the tools look for them, and everything else agent-only (handoff
prompts, plans, task lists) under `.agents\`. All of it is `export-ignore`
in `.gitattributes`, so `git archive` and GitHub's source zips leave it out.
`tools\check_agent_material.py` checks recognized agent instruction and
prompt paths for `export-ignore`, and rejects zero-width, bidi or tag
characters in repository text. Placement of other agent-only material under
`.agents\` is a rule for authors; filenames alone cannot identify every such
file.

- `docs\` is written for people and is not agent material, even where an
  agent wrote it.
- No agent-addressed text in source comments, commit messages or release
  notes: none of them can be stripped.

## The analysis directory

`analysis\` in the **main checkout** is its own git repository (private: `characterecho-sean/elite-analysis`),
cloned into this one and git-ignored here. It holds the decompiler and disassembly notes, the address maps and
the Ghidra scripts, and, kept local and never committed, the game executables for builds 332841, 334473 and
334925 and the Ghidra projects. **Read `analysis\AGENTS.md` before working in it**; it has its own rules and skills
(`elite-rva`, `retarget-build`, `ghidra-headless`, `record-finding`).

- A git worktree has no `analysis\`. Reach it by the main checkout's absolute path
  (`C:\Users\seanm\projects\edvr-unofficial-patch\analysis`; from a worktree, `git rev-parse --git-common-dir`
  names `<main>\.git`). **Never junction or symlink it into a worktree**: removing the worktree would delete
  the 4 GB of Ghidra projects through the link. `tools\build_diff.py` and the per-arc `pins.py` take its
  executables by path.
- It is a separate repository: commit there with `git -C analysis ...`, to its own `main`, and stage files by
  path. Its history is not EDVR's, and nothing in it ships in an EDVR archive.
- Source comments and docs here cite its files as `analysis\decomp\...`. Those files are not in this public
  repository: a citation is a pointer for the maintainer, not evidence a stranger can check, so state the
  fact in the doc as well, and do not paste decompiled code into this repository or its issues.
- **A session in a worktree does not write into `analysis\`.** The app stops Write and Edit there, and one
  analysis session, started from the main checkout, is the only writer: it reviews, files and commits what
  others hand over. Do not get around the guard with a shell write, a second clone or a junction. To hand over
  a finding, make a new folder `C:\Users\seanm\AppData\Local\EDVR\analysis-inbox\<yyyymmdd-hhmmss>-<topic>\`
  (outside every repository, so it survives your worktree), put the notes and scripts in it with a
  `MANIFEST.md` (build, from, summary, and for each file its destination under `analysis\`; the format and the
  checks are in `analysis\AGENTS.md`, "Intake"), then send the folder name to the session titled
  `elite-analysis intake` (`ListAgents`, or `mcp__ccd_session_mgmt__list_sessions`) if it is running. If it
  is not, say in your report that an item is waiting. Cite the file in your doc as `analysis\decomp\<name>`
  once it is filed. Reading `analysis\` needs no handover.

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
| `.agents\` | agent-only material: handoff prompts, plans; `export-ignore`, never in an archive |
| `analysis\` | main checkout only: the private `elite-analysis` repository (git-ignored here), see above |
| `build.bat` | full build, all gates; `--dll-only` is the receipt-guarded promotion path |
| `package.bat <version>` | packages a release from the last full build; it does not build, and refuses unless the build's receipt matches the tree (run `build.bat --wait-lock` first) |

# Coding standards: review

Apply these when reviewing a diff, your own included before you push it.
They are the judgement calls no gate can
make; the mechanical rules live in `build.bat`'s gates (the
`tools\check_*.py` scripts). Each rule
names the review that taught it. Every rule applied to every changed file is
the bar; report each finding with the file, the line and the rule.

## Proven claims

- **A command written in a doc or a PR has been run.** Run it, or capture
  the request it would send (`GH_DEBUG=api` against a dead localhost host
  with a dummy token sends nothing to GitHub), and check the result. A
  command that only looks right is a finding. (#103: the protection
  command's dotted `gh api` fields were sent as literal keys)
- **A claim about how GitHub, git, Windows or a library behaves cites its
  source** (the doc's URL, the CLI version's source line) or a reproduction.
  Memory is not a source. (#103: "required checks apply only to PR merges"
  was wrong and dismissed a valid option)
- **A fix comes with a test that fails without it.** Run the test on the
  unfixed code first, and say so. (#103: the rename fixture; the census glue
  test forced slow to show the flake)
- **Test the inputs a reviewer will try:** a rename or a delete for a file
  filter, a read interleaved with a write and a list read in parts, a
  timeout after the side effect may have happened, a host path that
  contains the string an assertion searches for. (#103, #107, #113)
- **Validation says what ran, on which commit, and what did not.** Name
  each check or rig, the head it ran on and its result. Name every skipped
  check and the reason. "Tests pass" with no list is a finding. (#107, #113)

## One owner

- **Reuse the owner of a lookup or an interpretation.** A second copy of an
  existing job drifts from the first. Installs resolve through
  `install_edvr.resolve_target` and `steam_library.py`; builds map through
  `tools\build_diff.py map`; the native build has one recipe,
  `.github\workflows\release.yml`. A change that re-derives what an owner
  already provides is a finding even when it works. (#121, #103)
- **Use the owner's vocabulary.** Status names, field names and terms come
  from the tool or doc that owns them (`build_diff.py`'s IDENTICAL,
  RELOC-ONLY, CHANGED), so a reader never maps one word to another. (#121)

## Shared state

- **A shared output never replaces.** A file published where another run,
  worktree or process can publish the same name (the main checkout's
  `analysis\`, a game directory) is published so that an existing file
  stops it: a hard link or an exclusive create, then compare. A rename that
  overwrites lets two racing runs both report success. (#121)
- **`--dry-run` writes nothing on every path the change touches**: no file,
  no temp file, no directory. Check each new write against it. (AGENTS.md,
  "The sanctioned tools")

## Every route

- **A behaviour added to a command with several routes covers every route**,
  with the route's own dry-run and verify-only meaning, and a self-test that
  drives each route through `main()` and asserts which ones carry it. Read
  every early `return` in the dispatcher. (#121: the direct native install
  returned before the archive step)

## Game addresses and offsets

- **Build-bound addresses live in `src\` and the arc's `docs\build-<build>.md`
  only.** A doc elsewhere that pins an address is stale at the next game
  update; name the function or field, the source file that binds it, and how
  to find it again. (#121)
- **Every struct offset names its base.** Game code often reaches camera
  fields through a helper base, camera+0x20, so one field has two offsets
  (`docs\camera-struct.md`). An offset given without its base is a finding.
  (#121: the projection kind written as camera+0x244, which is inside the
  cached view-projection matrix; the field is camera+0x264)

# Coding standards: review

Apply these when reviewing a diff. They are the judgement calls no gate can
make; the mechanical rules live in `build.bat`'s gates (the
`tools\check_*.py` scripts). Each rule
names the review that taught it. Every rule applied to every changed file is
the bar; report each finding with the file, the line and the rule.

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

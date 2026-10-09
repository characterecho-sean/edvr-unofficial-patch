# Flat loading-screen hologram ghost (HDR route, SS 1.0)

## Status

- Symptom: flat, Epic, DLSS, SS 1.0, 3840x2160. The loading screen's hologram ghosts and smears.
  DLSS off: gone. fix.ui_quality off: remains. SS < 1: gone.
- State: instrument built (this commit), not flown. No behaviour change yet.
- Evidence so far (log 141012, build 1f27a834), loading frames: HDR route treats 410 of 410 frames;
  `flat source` window says source-free-frames=450, source-free-treated=450 (section 104: selected with
  no pool draw, every pixel on the camera term); `flat camera rows` max-err 1e-7 (camera static, so the
  camera term gives zero motion); `flat runtime` accepted-history every frame, streak 1578, no reset.
  So DLSS gets zero motion for a rotating hologram and keeps accumulating: a ghost by construction.
  Section 104 assumed a source-free scene is "ground and sky, nothing moves the producer cannot see".
- Why SS < 1 is clean: the HDR route declines (does not evaluate at render size), the copy route's
  structure says no-scene, nothing treated, no DLSS.
- Open hypotheses, each with its signature (new line `flat source-free content 5s`):
  - A. the source-free frame's scene depth holds only hologram-family draws: hologram-only ~ frames.
  - B. nothing at all drew on the depth (empty-scene-depth ~ frames): the frame is a pure 2D screen.
  - C. a real world is present (other-draws ~ frames): A/B wrong, section 104 is not the cause.
  - D. DLSS AutoExposure with no exposure input misjudges a near-black frame (lead from the
    "Flat game display crash and UI smearing" session: branch claude/flat-display-crash-ui-smearing-f1d96a,
    temporary key advanced.flat_dlss_exposure = auto|fixed, flat hdr luma line). Test on the loading
    screen with their key; do not edit dlaa.cpp exposure here meanwhile.
  - E. unjittered hologram drawn into a jittered frame (shimmer, not smear): `flat camera rows` pairs
    are consistent, weak.
- Next flight: Epic, SS 1.0, sit on the loading screen 30 s. Read `flat source-free content 5s`.
  If A or B: fix = refuse (or reset history on) source-free frames with no world on the depth, by
  structure, not a threshold. If D: the exposure session's fix.
- Ruled out: the flat UI layer (idle on these frames, docs\design-flat-ui-quality-2026-10-05.md).

## Journal

- 2026-10-09: log read, instrument added (flat_runtime.cpp noteSourceFreeContent, log only; flat only).

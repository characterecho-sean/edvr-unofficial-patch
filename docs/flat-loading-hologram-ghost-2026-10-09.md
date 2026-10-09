# Flat loading-screen hologram ghost (HDR route, SS 1.0)

## Status

- Symptom: flat, Epic, DLSS, SS 1.0, 3840x2160. The loading screen's hologram ghosts and smears.
  DLSS off: gone. fix.ui_quality off: remains. SS < 1: gone.
- State: fix built (blank-scene decline in the HDR route), not flown. Flat only; VR untouched. No key (Sean: no A/B).
- Evidence so far (log 141012, build 1f27a834), loading frames: HDR route treats 410 of 410 frames;
  `flat source` window says source-free-frames=450, source-free-treated=450 (section 104: selected with
  no pool draw, every pixel on the camera term); `flat camera rows` max-err 1e-7 (camera static, so the
  camera term gives zero motion); `flat runtime` accepted-history every frame, streak 1578, no reset.
  So DLSS gets zero motion for a rotating hologram and keeps accumulating: a ghost by construction.
  Section 104 assumed a source-free scene is "ground and sky, nothing moves the producer cannot see".
- Why SS < 1 is clean: the HDR route declines (does not evaluate at render size), the copy route's
  structure says no-scene, nothing treated, no DLSS.
- Flight 151608 (0ce61cb5, FSR3 then DLAA; FSR3 ghosts worse than DLAA): source-free frames 215..449 per 5 s,
  empty-scene-depth=0, other-draws=100%. The depth holds ONE pair: VS 525D47E3D5E2EFF4 / PS 0D617929FED842F0, a
  full-screen texture filter with no projection (flat_projection_recipes.h: unchanged), and the camera origin reads
  (0,0,0). So: no world, only a screen filter; section 104 took it for ground and sky and gave the camera term (zero).
- ruled out: A/B as worded (hologram-only / empty depth): the depth holds a screen filter, not hologram draws and not
  nothing; the hologram family draws are not on that depth.
- ruled out: D (AutoExposure) as the main cause, because FSR3, with its own exposure path, ghosts worse than DLAA.
  Not excluded as a separate dimming cause (other session's branch).
- Fix: a selected source-free scene whose depth holds only unchanged full-screen filters (or nothing) is blank;
  treatHdr declines it ("blank-scene"), the copy route's no-scene verdict leaves it untreated, as at SS < 1.
  Ground and sky have geometry draws on that depth, so they stay treated. Log: `flat source-free content 5s`
  (filter-only, other-draws, blank-scene-declined).
- Next flight: loading screen + Pilot's Handbook at SS 1.0 (FSR3, DLAA, DLSS): ghost gone? blank-scene-declined ~ frames?
  Also check a settlement / open-ground walk: source-free frames still treated (other-draws high, declined 0).
- Ruled out: the flat UI layer (idle on these frames, docs\design-flat-ui-quality-2026-10-05.md).

## Journal

- 2026-10-09: log read, instrument added (flat_runtime.cpp noteSourceFreeContent, log only; flat only).

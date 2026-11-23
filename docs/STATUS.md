# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11, 13, 15, 16, 18 and 19 accepted; 12 (clouds) paused; 17 (far terrain) tasks 1–5 seen | Milestone 14 (run-time sun) built, awaiting the owner's look; 21 (tonemap and grade) task 1 built | [render.md](lanes/render.md#14-run-time-sun-term-the-lightmaps-sun-channel-dropped) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q, R fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 14, run-time sun term

Tasks 0–4 built and checked by an agent (2026-11-23); design, as built and what was seen in
[render.md](lanes/render.md#14-run-time-sun-term-the-lightmaps-sun-channel-dropped). Open: the
owner's look; frame time with the final GPU pass; 5 on acceptance (the baked fallback and
`renderer.sunBakedFallback` deleted). Left out, for the owner: overgrowth in the static cascade
(it streams round the camera), static meshes placed during a round (deployables).

## In progress: `render` milestone 21, tonemap and grade

Tasks 1 (the GT curve, white balance, `renderer.grade*`), 2 (tint and saturation per range) and 4 (low light) built; 3 (the LUT) deferred with per-level looks (owner, 2026-11-23)
([render.md](lanes/render.md#21-tonemap-and-grade)). `renderer.gradeCurve 0` is milestone 13's grade.

## Paused: `render` milestone 17, far terrain

Tasks 1–5 built and seen by the owner (2026-11-23: fine for now); Kunar Province, Kashan Desert and
Muttrah City baked. Next: 6, bakes for the other levels; 7, judged from a jet, cost with the final
GPU pass ([render.md](lanes/render.md#17-far-terrain)).

## Paused: `render` milestone 12, clouds

Tasks 1–7 and 8's day sky done, 6 judged by the owner (2026-11-22; GPU time with the final GPU pass)
([render.md](lanes/render.md#12-clouds)). Open: 8's night layers (stars from a catalog with the moon, the
sky's rotation; the Yale Bright Star Catalogue has no stated licence, so it waits on the owner: a
catalog read from outside the tree, or another source); 9 (low scud, perhaps).

Reference views (`view.save` by the owner; milestone 11's shots `m11acc-<view>`, earlier
`m10-<view>` at 1600x900): Muttrah City and Fallujah West `sun`, `antisun`, `ridges`, `valley`,
`street`; Saaremaa the same with `water` for `street`; test_bootcamp (`gpm_cq 16`) `gloss`,
`gloss2`, `glass`. A view facing the sun has yaw = the survey's sun azimuth + 180°.

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it.

- The grade (milestone 21, tasks 1, 2, 4, 6): `renderer.gradeCurve 1` the new grade (default: contrast
  1.2, toe 1.5, the rational shoulder), 0 the old Neutral grade. Auto-exposure lifts dark scenes only
  partly, `renderer.exposureStrength` (0.6; 1 the old full lift): to judge indoors (no interior view
  saved). Open before acceptance: a fire. Muttrah City `sun`, `street`; The Falklands `gpm_cq 32` `w0` (night as accepted, the toe
  relaxed past the exposure ceiling, a faint grey-blue by `renderer.gradeNight`). Also
  `gradeHighlightShift` (0.5) at a fire, `gradeTemperature`/`gradeTint`, `gradeShoulder`;
  per range `gradeShadowTint`, `gradeMidTint`, `gradeHighlightTint` (`r/g/b`), `gradeShadowSaturation`,
  `gradeHighlightSaturation`.

- The run-time sun (milestone 14): `renderer.sunBakedFallback 1` is the old baked sun past the
  cascades, 0 (the default) the new. Kashan Desert, `lightManager.sunDirection 0.7/-0.07/0.71`
  (4°), views `kas_high`, `kas_here`, `m14-above` (buildings from above; `renderer.sunShadows 0`
  shows the static cascade alone); Muttrah City `sun`, `ridges` at the level's sun; Saaremaa
  `water`. `tools.sun` dumps the layers.

- Clouds (milestone 12, contracts 32–33): no step at the 16 km handoff and no far radial edge
  (one air integral, a cross-fade band); edge albedo and the eroded light bake. Kashan Desert,
  views `hb-owner`, `hb-owner-back`, sun `lightManager.sunDirection 0.7/-0.07/0.71`.
- Clouds' ambient along the view (contract 34): shaded sides take the sky behind them. The dusk
  flatness was Kashan's haze (render.md, 12, near clouds); compare with `renderer.weatherVisibility
  60000` and `renderer.weatherMixing 1500` (the level's: 20000, 3500).
- The sky under the deck (contract 35): gaps between clouds now take the clouds' shadow on the
  haze below the deck, so they no longer glow clear past veiled clouds. Same views.

## Open issues

- Far clouds at dusk shade flat under Kashan's dust haze (visibility 20 km, mixing 3500 m): the
  air's in-scatter, as modelled; whether that haze is right for dusk (a deep dust layer at
  sunset) is the owner's call ([render.md](lanes/render.md#12-clouds), near clouds' notes).

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

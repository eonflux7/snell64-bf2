# Status

Where each lane stands now: an index. Read this first, then only the document it links to. Update
it in the commit that changes a lane's or a milestone's state. A milestone's tasks and their state
live only in its document in [milestones/](milestones/); the row here gives the next step.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11, 13–16, 18 and 19 accepted | 21, 22, 23 in progress; 12, 17 paused ([below](#open-milestones)) | [render/](render/README.md) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q, R fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Open milestones

Several may run at once; each session works in one milestone's document.

| Milestone | State | Next | Document |
|---|---|---|---|
| `render` 23, water at the shore | In progress: tasks 1–6 and 7's current built (2026-11-24) | 7's standing waves and river foam; 8 | [render-23-water-shore.md](milestones/render-23-water-shore.md) |
| `render` 22, far overgrowth | In progress: task 1 built (2026-11-23) | 3 (shadows), 4 (index range), 5 (checks) | [render-22-far-overgrowth.md](milestones/render-22-far-overgrowth.md) |
| `render` 21, tonemap and grade | In progress: tasks 1, 2, 4, 6 built; 3 (the LUT) deferred with per-level looks (owner, 2026-11-23) | 5 (checks; a fire) | [render-21-grade.md](milestones/render-21-grade.md) |
| `render` 17, far terrain | Paused: tasks 1–5 seen by the owner (2026-11-23, fine for now) | 6 (bakes for the other levels), 7 (from a jet, cost) | [render-17-far-terrain.md](milestones/render-17-far-terrain.md) |
| `render` 12, clouds | Paused (owner, 2026-11-22): tasks 1–7 and 8's day sky done | 8's night layers (a star catalog: the owner's call), 9 | [render-12-clouds.md](milestones/render-12-clouds.md) |

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it.

- Water's dark shore patches (milestone 23, task 1): the refracted bed read, not SSR. Muttrah City
  `ssr-shore`.
- The open-water map (milestone 23, task 2): waves, whitecaps and the shore's breaking depth by the
  fetch upwind; `renderer.waterOpen 0` the level's sea everywhere. The wind blows from the map's most
  open sea (`renderer.waterWindFrom` a bearing to override; `WaterScroll` no longer the wind), the
  lee fills in by wave energy, diffracting round the breakwaters. Muttrah City `ssr-shore` and the
  harbour from above (where the line from the pier's corner showed): wind from 60°.
- Shore waves (milestone 23, task 3): the level's peak wave refracted by the bed, shoaling and
  breaking in bands up the beach, foam from the break in place of the fixed strip. Omaha Beach
  `surf`, `surf-along`; Muttrah City `beach-e`.
- The swash (milestone 23, task 3, contract 41): each bore runs up the sand to its own lobed line
  and drains back, the backwash uncovering the bed below the still line; wet sheet, draining film,
  damp sand. Omaha Beach `swash` (at the waterline, into the sun), `surf-along`.
- Water types (milestone 23, task 4, contract 42): the water's colour from its constituents, a
  type per level in its weather file; `renderer.waterType` 0–10 (oceanic I to coastal 9, 10 a
  glacial river). Muttrah City `ssr-shore` (coastal 1; try 0 and 9), Kunar `river` (glacial river).
- Surf turbidity (milestone 23, task 5, contract 43): breaking waves stir sediment into the surf
  zone, khaki and opaque, falling offshore in plumes; sand only. `renderer.waterSurfSediment` (40
  g/m³ per metre of breaker height; 0 off). Omaha Beach `surf-along`, `surf-plume`.
- Caustics (milestone 23, task 6, contract 44): the sun focused by the waves on the bed,
  fading with depth and the water's scattering; `renderer.waterCaustics` (1; 0 off, up to 4).
  Omaha Beach `caustics` (with `lightManager.sunDirection 0.3/-0.9/0.3`, `renderer.waterType 0`,
  `renderer.waterSurfSediment 0`).
- The rivers' current (milestone 23, task 7, contract 45): ripples and foam carried downstream,
  fast in the narrows, still at the banks. Kunar `river` (`renderer.waterFlowTo -1` still water,
  225 the level's; `renderer.waterFlowSpeed` m/s, 0 from the width: 0.93).
- Source effects with a stale cache compile on workers (`perf`, `BF2_PERF_FXASYNC=0` the old
  path): the old bytecode draws until the new one swaps in.
- Far overgrowth (milestone 22, task 1): trees over the whole map, no setting. Kunar `og-sw`,
  `ft-1500`, `edge-e`.
- The grade (milestone 21, tasks 1, 2, 4, 6): `renderer.gradeCurve 1` the new grade (default: contrast
  1.2, saturation 1.15, exposure bias −0.3, toe 1.5, the rational shoulder), 0 the old Neutral grade. Auto-exposure lifts dark scenes only
  partly, `renderer.exposureStrength` (0.6; 1 the old full lift): to judge indoors (no interior view
  saved). Open before acceptance: a fire. Muttrah City `sun`, `street`; The Falklands `gpm_cq 32` `w0` (night as accepted, the toe
  relaxed past the exposure ceiling, a faint grey-blue by `renderer.gradeNight`). Also
  `gradeHighlightShift` (0.5) at a fire, `gradeTemperature`/`gradeTint`, `gradeShoulder`;
  per range `gradeShadowTint`, `gradeMidTint`, `gradeHighlightTint` (`r/g/b`), `gradeShadowSaturation`,
  `gradeHighlightSaturation`.

- Clouds (milestone 12, contracts 32–33): no step at the 16 km handoff and no far radial edge
  (one air integral, a cross-fade band); edge albedo and the eroded light bake. Kashan Desert,
  views `hb-owner`, `hb-owner-back`, sun `lightManager.sunDirection 0.7/-0.07/0.71`.
- Clouds' ambient along the view (contract 34): shaded sides take the sky behind them. The dusk
  flatness was Kashan's haze ([render-12-clouds.md](milestones/render-12-clouds.md), near clouds); compare with `renderer.weatherVisibility
  60000` and `renderer.weatherMixing 1500` (the level's: 20000, 3500).
- The sky under the deck (contract 35): gaps between clouds now take the clouds' shadow on the
  haze below the deck, so they no longer glow clear past veiled clouds. Same views.

## Open issues

- Far clouds at dusk shade flat under Kashan's dust haze (visibility 20 km, mixing 3500 m): the
  air's in-scatter, as modelled; whether that haze is right for dusk (a deep dust layer at
  sunset) is the owner's call ([render-12-clouds.md](milestones/render-12-clouds.md), near clouds' notes).

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

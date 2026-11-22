# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11 and 15 accepted | Milestone 12 (clouds): tasks 1–4 built; task 5 (far noise, by eye) built | [render.md](lanes/render.md#12-clouds) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 12, clouds

Planned (owner, 2026-11-22; [render.md](lanes/render.md#12-clouds)); as built so far there.

1. Cloud material (`RaCloud.fxh`: delta-scaled droplet phase, octaves) and the cloud light table: built
   (agent: compiled, table dumped on Kashan Desert); nothing draws with it before task 3.
2. Weather map and baked volumes (`Clouds.cpp`, `RaClouds.fx`, contract 13; `renderer.cloudPreset`,
   `renderer.cloudCoverage`): built (agent: `tools.clouds` on Kashan Desert at 0.3/0.7/1.0); high
   cloud's flat layer moves to task 3 with the panorama that draws it.
3. Cloud panorama, high cloud and wind (contract 14; `renderer.clouds`): built (agent: Kashan
   Desert at coverage 0.3/0.4/1.0, ground and 600 m, `tools.clouds`).
4. Reachable clouds (contract 15; `renderer.cloudNear`): built (agent: Kashan Desert from the
   ground, 3, 4.5 and 8 km, coverage 0.4 and 1.0; views `m12t4-*`). The GPU budget is set later
   (owner); the cost is measured in task 6.
5. Far cloud noise and shimmer, tweaked by eye (owner, 2026-11-22, in place of the reference set):
   the panorama accumulated over panoramas: built (agent: Kashan Desert `m12-up90`, the grain
   gone and steady between shots).
6. Prototype judged by eye on Kashan Desert (ground, air, inside a cloud; coverage 0.3/0.7/1.0);
   its GPU time measured.
7. Coverage drives the lighting (sun, shadows, sky light, haze), checked first against a real
   overcast reference's SH. The haze and sky light are dimmed by the sun ray's length through the
   cloud slab times coverage (Dagor's `statistical_clouds_shadow`).
8. Cloud shadows on the ground (Beer shadow maps, Hillaire 2020) and into the sky cube; then the
   night layers: stars from a catalog with the moon, the sky's rotation from latitude and sidereal
   time (Dagor's `daStars`, `daSkiesAstronomy`; the catalog's source and licence checked first).
   PR's sun direction is kept (baked lightmaps follow it); a date is fitted to it.
9. Low scud under overcast (perhaps; render.md).

Reference views (`view.save` by the owner; milestone 11's shots `m11acc-<view>`, earlier
`m10-<view>` at 1600x900): Muttrah City and Fallujah West `sun`, `antisun`, `ridges`, `valley`,
`street`; Saaremaa the same with `water` for `street`; test_bootcamp (`gpm_cq 16`) `gloss`,
`gloss2`, `glass`. A view facing the sun has yaw = the survey's sun azimuth + 180°.

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it.

- Device reset crash in the first frame (stale `RaShadowViewProj` handle): fixed in `717b2671`,
  24 resets without a crash (agent, Muttrah City).
- Sun shadows: far overgrowth cells cast (`980b32aa`), blend to baked by camera distance; loaded on
  Saaremaa.
- `tools` milestone 1 tasks 4–5 (Views tab, `view.shot`).
- Sky clouds (render milestone 12, tasks 1–3): Kashan Desert, views `m12-up`, `m12-up90`,
  `m12-up180`, `m12-up270`, `kas_high`; `renderer.cloudCoverage` 0.3/0.4/1.0, `renderer.cloudPreset`
  0/1, `renderer.clouds 0` for the clear sky.
- Reachable clouds (render milestone 12, task 4): Kashan Desert, views `m12t4-base` (3 km),
  `m12t4-in` (4.5 km; `renderer.cloudCoverage 1` puts the camera in the deck), `m12t4-above`,
  `m12t4-above2` (8 km), `m12-up`; `renderer.cloudNear 0` for the panorama alone.
- Far clouds without grain or shimmer (render milestone 12, task 5): Kashan Desert, `m12-up90`
  (the horizon band).

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

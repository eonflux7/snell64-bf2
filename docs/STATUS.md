# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11 and 15 accepted | Milestone 12 (clouds): tasks 1–5, 7 built; task 8's cloud shadows built, its night layers wait on the star catalog's licence | [render.md](lanes/render.md#12-clouds) |
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
   the panorama accumulated over panoramas, then 2048x1024 read with Catmull-Rom: built (agent:
   Kashan Desert `m12-up90`, the grain gone, sharper, steady between shots; cost not measured).
   Desert clouds lowered to 2 km (was 3.5; owner, by eye). Two layers with their own heights
   (Dagor's): desert cumulus 2–6 km and an altocumulus sheet 6.5–8 km; `renderer.cloudBase`,
   `cloudTop`, `cloudMidBase`, `cloudMidTop`, `cloudMidDensity`, `cloudMidCoverage` (agent: Kashan
   Desert `m12-up`, `m12-up180`, the mid layer alone at coverage 0.5).
6. Prototype judged by eye on Kashan Desert (ground, air, inside a cloud; coverage 0.3/0.7/1.0);
   its GPU time measured.
7. Coverage drives the lighting (contract 16): built (agent: Kashan Desert, coverage 0.3/0.7/1.0
   from the ground and 600 m; checked against the CIE overcast sky and Kasten–Czeplak,
   [render.md](lanes/render.md#12-clouds)). The sky cube and sky light from a ground panorama of
   the clouds; the haze, the air in front of the clouds and the horizon under the deck lit by
   the clouds (`Ra_HazeLight`).
8. Cloud shadows on the ground (Beer shadow map, Hillaire 2020; contract 16) and the clouds in
   the sky cube: built (agent: Kashan Desert, `kas_high` at 0.4 and 1.0). Then the night layers:
   stars from a catalog with the moon, the sky's rotation from latitude and sidereal time
   (Dagor's `daStars`, `daSkiesAstronomy`). PR's sun direction is kept (baked lightmaps follow
   it); a date is fitted to it. Catalog checked (agent, 2026-11-22): the Yale Bright Star
   Catalogue has no stated licence (HEASARC's terms of use; Debian ships it in non-free), so it
   cannot go in this tree as is; waits on the owner (a catalog read from outside the tree, or
   another source).
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
- Far clouds without grain or shimmer, sharper (render milestone 12, task 5): Kashan Desert,
  `m12-up90` (the horizon band).
- Two cloud layers, desert base 2 km (task 5): Kashan Desert `m12-up180`; `renderer.cloudCoverage 0`
  with `renderer.cloudMidCoverage 0.5` shows the mid layer alone.
- Cloud shadows and coverage-driven light (tasks 7–8): Kashan Desert `kas_high` (shadows on the
  ground), `kas_here` and `m12-up` at `renderer.cloudCoverage` 0.3, 0.7 and 1.0 (1.0 a grey storm
  deck); `renderer.clouds 0` for the clear sky's light. `tools.clouds` writes `-shadow.png`.

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11 and 15 accepted; 12 (clouds) paused | Milestone 16 (water): task 1 built | [render.md](lanes/render.md#16-water) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 16, water

Planned (owner, 2026-11-22; [render.md](lanes/render.md#16-water)); as built there.

1. The ring around the camera (the lightmap's sun term read as visibility past the cascades) and
   the reflections' streaks (false hits behind thin objects): built (agent: The Falklands,
   views `wy316` toward the sun, `wy136` the islands, clouds off).
2. Wave spectrum: three FFT cascades on the GPU (engine `Water.cpp`, a new contract), the shader
   reading them in place of the 3D normal volume.
3. Filtered normals (LEAN) and a GGX sun lobe from the slope variance.
4. The water body: refraction, depth from the scene, absorption and in-scatter, crest light.
5. Foam at shores and on crests.
6. Geometry: displacement near the camera or normals only (owner's choice).
7. Judged by eye on The Falklands and Saaremaa; GPU cost measured.

## Paused: `render` milestone 12, clouds

Tasks 1–5 and 7–8 built ([render.md](lanes/render.md#12-clouds)). Open: 6 (the prototype judged on
Kashan Desert, its GPU time measured); 8's night layers (stars from a catalog with the moon, the
sky's rotation; the Yale Bright Star Catalogue has no stated licence, so it waits on the owner: a
catalog read from outside the tree, or another source); 9 (low scud, perhaps).

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

- Water's ring and reflection streaks (render milestone 16, task 1): The Falklands, views `wy316`
  (toward the sun, the glint now reaches the horizon) and `wy136` (the islands' reflections).

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

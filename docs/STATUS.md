# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–11 and 15 accepted; 12 (clouds) paused | Milestone 16 (water): tasks done; acceptance cleanup | [render.md](lanes/render.md#16-water) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q, R fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 16, water

Planned (owner, 2026-11-22; [render.md](lanes/render.md#16-water)); as built there.

1. The ring around the camera (the lightmap's sun term read as visibility past the cascades) and
   the reflections' streaks (false hits behind thin objects): built (agent: The Falklands,
   views `wy316` toward the sun, `wy136` the islands, clouds off).
2. Wave spectrum: three FFT cascades on the GPU (`Water.cpp`, contract 17; `renderer.waterWaves`,
   `waterWind`, `waterFetch`): built (agent: The Falklands `wy316`, `wlow`; animates).
3. Filtered normals (LEAN) and a GGX sun lobe from the slope variance: built (agent: The Falklands
   `wy316`, `wlow`; a smooth glitter path, no sparkle up close), and rough reflections from the
   same variance (agent: `wy136`, `wy316`, `wlow` A/B, clouds off). The far sea reads green until
   task 4's body; the path clips to white until milestone 13's tonemapping.
4. The water body: refraction, depth from the scene, absorption and in-scatter, crest light: built
   (agent: The Falklands `wy136`, `wlow`, `wy316` against task 3, clouds off; the crest light not
   seen yet, it wants a low sun). Found on the way: fix R (16 KB pixel shader buffer).
5. Foam at shores and on crests: built (agent: The Falklands `wlow` at 7 and 15 m/s, `wy136`, `wshore`
   (the small island zoomed), `whigh`, `wy316`; `renderer.waterFoam` 0 for none); reworked after
   the owner's look for tiling and aliasing.
6. Geometry: normals only for now (owner, 2026-11-22).
7. Judged by eye on The Falklands and Saaremaa: done (owner, 2026-11-22). GPU cost with the final
   GPU pass. Left for acceptance: delete `renderer.waterWaves 0` (retail's normals) and its path.

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

Built and checked by an agent; an entry goes when the owner has seen it. Empty: everything up to
water's foam was ticked (owner, 2026-11-22).

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

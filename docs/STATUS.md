# Status

Where each lane stands now. Read this first, then only the section it links to. Update it in the
commit that changes a lane's state. It holds state, not history: a finished task is one line, its
design and as-built facts live in the lane's document, the story in commit messages.

| Lane | State | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–10 accepted; 11 in progress (tasks 1–5, 7–12 built); 15 (shader pipeline) built, awaiting the owner | Milestone 11 tasks 11 and 12, awaiting the owner's look | [render.md](lanes/render.md#11-atmosphere-sky-ibl-water-reflections) |
| `tools` | Milestone 1 tasks 1–5, milestone 3 (cut) built | Paused; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](lanes/ui.md) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost done by render milestone 9 | Parked | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [fix.md](lanes/fix.md) |

## Current: `render` milestone 11, atmosphere, sky IBL, water reflections

Design, decisions and what is built: [render.md](lanes/render.md#11-atmosphere-sky-ibl-water-reflections).
One A/B console variable per part while in development (`renderer.atmosphere`,
`renderer.viewDistance`, `renderer.waterSsr`); all
deleted with the old paths at acceptance. The owner judges each A/B by eye against the plan's
"what should change" list; a task that works but does not look better is not done. Every task that
adds a frame resource is tried across device resets (`renderer.vsync` toggled while it draws).

Reference views (`view.save` by the owner, baseline shots `m10-<view>`): Muttrah City and Fallujah
West `sun`, `antisun`, `ridges`, `valley`, `street`; Saaremaa the same with `water` for `street`;
test_bootcamp (`gpm_cq 16`) `gloss`, `gloss2`, `glass`. A view facing the sun has yaw = the
survey's sun azimuth + 180°.

Tasks:
1. Survey (`tools.survey`) and reference views. Done.
2. Atmosphere model and tables. Built; checked by an agent with `tools.atmosphere`.
3. Sky and sun (`RaSky`). Done (owner: "looks good"). Under water and thermals deferred by the
   owner.
4. Aerial perspective. Done (owner, Kashan Desert).
5. View distance. Built; the owner's look on four maps: good. Left items: render.md's "Built so
   far".
6. Light shafts. Dropped for now (owner, 2026-11-21); not part of milestone 11's acceptance.
7. Sky cube (s10), SH9 irradiance (`RaSkySH`, c192–c198), split-sum BRDF table (s11). Built;
   checked by an agent with `tools.atmosphere`. Retail environment maps deleted at acceptance
   where nothing else reads them.
8. Shared lighting model, one physical light (contract 11). Built; bundled meshes wired with a
   placeholder class; the owner's look on test_bootcamp: better. Foliage and particles' sky
   light built (agent), judged with task 10. Left: point lights against the new sun.
9. Bundled meshes (judged on test_bootcamp). Done (owner: looks good). Left: render.md's
   "Built so far".
10. Static meshes. Done (owner: looks ok).
11. Soldiers (hemi map), decals and terrain. Built: soldiers and terrain (agent, Saaremaa `valley`,
    shots `t11-valley-old`/`-new`: subtle on its gentle slopes); soldiers not seen in third person
    by the agent. Decals left unoccluded (render.md's "Built so far"). Awaiting the owner's look.
12. Water SSR (`renderer.waterSsr`). Built (agent, Saaremaa `water`, shots `t12-water-nossr`/`-ssr`:
    the shore and treeline reflected, the sky cube nearer the camera). Awaiting the owner's look.
    Look for: buildings and shore reflected.
13. Acceptance: the owner's look at the reference views on the three maps against milestone 10;
    the night layer loads and looks no worse; benches against milestone 10
    ([perf-frame.md](lanes/perf-frame.md#current)) within the 1 ms budget, or the regression
    accepted by the owner; GPU pass times; old paths and variables deleted; milestone 11's
    section in render.md rewritten as built.

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it.

- Shader pipeline (render milestone 15, [render.md](lanes/render.md#15-shader-pipeline)): effects
  compile with `d3dcompiler_47`. test_bootcamp to in game: every effect stale 30 s (was about 10
  min), after a bm shader edit 22 s, warm 17 s; views `gloss`, `gloss2`, `glass` unchanged.
  `d3dx9_43` is now the only D3DX (`d3dx9_24`/`_25` gone, `BF2_FX_COMPILER` deleted): Muttrah City,
  Fallujah West and Saaremaa load, views and frame time as with `d3dx9_25`. Plain shaders in place
  of D3DX effects deferred to a move off D3D9.
- Atmosphere tab (`tools.atmosphere`, milestone 11 task 2).
- Sky cube (milestone 11 task 7): `tools.atmosphere NAME` writes `NAME-skycube.png` (faces across,
  mips down) on Muttrah City; nothing in the scene reads it until task 8.
- Device reset crash in the first frame (stale `RaShadowViewProj` handle): fixed in `717b2671`,
  24 resets without a crash (agent, Muttrah City).
- Sun shadows: far overgrowth cells cast (`980b32aa`), blend to baked by camera distance; loaded on
  Saaremaa.
- `tools` milestone 1 tasks 4–5 (Views tab, `view.shot`).

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (owner idea, 2026-11-19): keep the far cascades' static casters while the
  snapped cascade holds still, draw moving objects over a copy. Not planned yet.

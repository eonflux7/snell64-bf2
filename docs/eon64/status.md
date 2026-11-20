# Status

Where each lane stands. Read this first; the Detail column leads to each lane's plan or as-built
document. Update it in the commit that changes a lane's state: a task done, something seen in game,
an open issue added or closed. When a milestone is accepted, its section here moves on to the next.

| Lane | Done | Next | Detail |
|---|---|---|---|
| `render` | Milestones 1–10 accepted | Milestone 11, atmosphere, sky IBL, water reflections (view distance added as task 5): task 2 built (owner's look pending), task 3 (sky and sun) seen; task 4 (aerial perspective) done; task 5 (view distance) in progress | [renderer.md](renderer.md#roadmap) |
| `tools` | Milestone 1: tasks 1–3 | Milestone 3 cut to the benchmark path; milestone 1 tasks 4–5 built | [tools.md](tools.md#milestones) |
| `ui` | Milestones 1–4 accepted | — | [ui.md](ui.md#milestones) |
| `perf` (load) | Trace, caches, prefetch, worker compiles; flags retired | — | [perf-load.md](perf-load.md#state) |
| `perf` (frame) | `/O2` plugins, `tailField` table; per-draw cost taken by render milestone 9 | Parked | [perf-frame.md](perf-frame.md#plan) |
| `compat` | See the list | Parked (owner, 2026-11-19); rhooks client features when resumed | [compat.md](compat.md#missing) |
| `fix` | G–O, Q fixed | A, C–F open (packaging, build) | [upstream-fixes.md](upstream-fixes.md) |

## Current: `tools` milestone 3, benchmark path (cut)

Order (owner, 2026-11-19, replacing "milestones 1–3 first"): views and `view.shot` are enough for
still images; milestone 9's frame-rate loss shows while the camera moves (streaming, LOD switches,
culling), so a repeatable camera path with a frame-time CSV comes next, then render milestone 9.
Plan: [tools.md](tools.md#3-benchmark-path). Left for later: milestone 1 tasks 6–8, milestone 2,
and milestone 3's history file and acceptance check (the spread between runs is still reported).

Tasks (1–3 done: paths, playback, their Views tab section and `bench.run` seen in game by the
owner, 2026-11-19, Muttrah City):
1. Paths in the view file: `path NAME` with indented `key TIME pos=… yaw=… pitch=… fov=…` lines,
   parsed and written like views; `path.key NAME TIME` appends a keyframe at the current camera.
2. Playback: `path.play NAME`, `path.stop`; real time, Catmull-Rom position, smoothed angles and FOV.
3. `bench.run PATH [runs] [label]`: a warm-up pass, then the runs with vsync and the frame cap off
   (restored after); per-frame CSV (frame, CPU frame time, GPU time per pass, draws) and a summary
   (average, 1 % low, worst, per-pass averages, spread between runs, CPU or GPU bound) in
   `<tools.outputDir>/<map>/bench/`. Tried: path `new` (10 s), 3 runs at 11.57 ms (86 fps), spread
   0.84 % on average and 18 % on 1 % low, every frame CPU bound, no GPU result dropped; vsync
   restored; the cap bypassed (`game.lockFps 50` ran at 75 fps); `bench.stop` writes nothing.
4. One path each on Muttrah City and Saaremaa: `bench1` on both (20 s, recorded by the owner,
   2026-11-19, Conquest 64). Saaremaa baseline, 3 runs: 8.12 ms (123 fps), 1 % low 11.75 ms,
   CPU bound, spread 5.26 % on average and 9.14 % on 1 % low (above the 2 % / 5 % target).
   Fallujah West added by the owner (worst map, ~35 fps in play), path `bench`: 25.50 ms
   (39 fps), 1 % low 32.05 ms, CPU bound, spread 2.71 % / 2.85 %; pass spans largest in the
   depth prepass (6.2 ms), world opaque (4.8) and sun cascade 3 (3.7), CPU submission included.
   Muttrah City, `bench1`: 13.37 ms (75 fps), 1 % low 18.48 ms, CPU bound, spread 1.16 % /
   1.83 % (within target); depth prepass 3.2 ms, world opaque 3.0, sun cascade 3 1.2.
   Those draw counts (146, 313, 377) left out every queued mesh; counted since `52868e8c`:
   Muttrah City `bench1` 8927 draws a frame (1 run, 14.00 ms, 2026-11-19); Fallujah West `bench`
   16085 (1 run, 26.11 ms) and Saaremaa `bench1` 4464 (1 run, 8.36 ms), engine `aa754216`,
   2026-11-19.

## Paused: `tools` milestone 1, views

Plan: [tools.md](tools.md#1-views).

Found (2026-11-19):
- Hook: `BF2Render::render` (`Game/Main/BF2Render.cpp`), after `setFarPlane` and before the
  `renderFrame` loop. Everything that culls, picks LODs, streams or sets up cascades reads the view
  after that point. On the spawn screen the game's camera is the player's `MultiPlayerFreeCamera`;
  with no player nothing sets the view, so the override must not depend on one.
- `RenderView`: `setViewMatrix` takes camera-to-world (rows right, up, forward, position; y up,
  left-handed); `setFov` is vertical, radians; `setAdjustedFov` widens for widescreen.
- HUD and spawn menu: both gated by `renderer.drawHud`.
- Pause: `ServerGameLogic::setPause` (game status 4) stops triggers, objects and physics and pauses
  particles; it has no console binding. Renderer time is one input, `renderFrame`'s `time`, so a
  freeze passes 0 there while a view is active.
- Screenshot: `renderer.dumpScreen` reads the back buffer at end of scene, after the HUD and before
  ImGui.

Tasks (1–3 done: seen in game by the owner, 2026-11-19; 4–5 built, not yet seen by the owner; 5
tried on Muttrah City by an agent: a view shot before the HUD and one with `tools.shotHud 1`):
1. Camera override: `view.*` state in a `[TOOLS]` module, applied at the hook; HUD hidden through
   `renderer.drawHud` and restored on leaving; idle is one test.
2. Free fly: `view.fly`, F8, keyboard and mouse read directly, wheel FOV, no collision.
3. View file: parse and minimal-diff write of `settings/eon64-views/<map>.txt`; `view.go`, `save`,
   `delete`, `leave`, `list`, `reload`; `run` lines with variable restore.
4. Views tab in the debug menu.
5. `view.shot`: scene read back before the HUD, PNG plus 960-wide JPEG into `tools.outputDir`.
6. Settle and freeze: pending streaming and crossfades, `tools.settleFrames`,
   `tools.settleTimeout`; renderer time frozen; local server paused through `setPause`.
7. Stamping: engine and shader commits, map, mode, layer, resolution, MSAA, vsync, frame cap,
   non-default `renderer.*` variables.
8. Determinism check: two shots of a view, the difference against a stated tolerance, its sources
   named in the report.

## Current: `render` milestone 11, atmosphere, sky IBL, water reflections

The goal is a visibly more realistic image; plan, design and decisions (the model's sky, clear
until milestone 12's clouds; authored sun colour kept; fog end stays the visibility limit):
[renderer.md](renderer.md#11-atmosphere-sky-ibl-water-reflections). One A/B console variable per
part while in development (`renderer.atmosphere`, `renderer.lightShafts`, `renderer.skyIbl`,
`renderer.waterSsr`); old paths and the variables are deleted at acceptance. Intermediate tasks:
one map, A/B shots at the reference views, no `GPU=1` runs. The owner judges each A/B by eye in
game, on the plan's list of what should change; a task that works but does not look better is not
done. Every task that adds a frame resource is tried across device resets (`renderer.vsync`
toggled while it draws).

Tasks (1 done 2026-11-20: survey, reference views and their baseline shots):
1. Survey, three maps and one night layer (The Falklands, `gpm_cq 32`), by a `tools` console command
   `tools.survey` (its own commit): sky dome technique and textures (how much is painted into the
   dome; its horizon and zenith colours against the fog colour), fog colour, start and end, sun and
   sky colours, sun elevation; the terrain colormap's mean colour; water effects in use and their
   area on screen; retail environment maps (the level's `Envmaps/EnvMap0`, the `EnvMapManager`
   maps: which templates, how many, when generated, who samples them); material flags that can sort
   bundled and static materials into paint, glass, metal, fabric (env map, alpha, gloss source,
   technique); whether normal-map mips were renormalized (Toksvig needs their shortened normals);
   the highest sampler each effect uses, to find three free slots. Reference views per map: the
   horizon toward and away from the sun, ridges and a valley at range, a glossy vehicle, glass,
   water, a street with sun between buildings. Baseline: milestone 10's benches and shots of those
   views.
   Done 2026-11-20: `tools.survey` (`tools(survey)` commit, in game) run on the three maps, the night layer and
   test_bootcamp; findings in [renderer.md](renderer.md#11-atmosphere-sky-ibl-water-reflections).
   Reference views (placed by the owner with `view.save`; baseline shots `m10-<view>` by `view.shot`):
   test_bootcamp done 2026-11-20 on `gpm_cq 16` (China vs Russia, modern vehicles): `gloss`, `gloss2`,
   `glass`. Muttrah City done 2026-11-20 (`gpm_skirmish 16`): `sun`, `antisun`, `ridges`, `valley`,
   `street`. Saaremaa done 2026-11-20 (`gpm_skirmish 16`): `sun`, `antisun`, `ridges`, `valley`,
   `water`. Fallujah West done 2026-11-20 (`gpm_skirmish 16`): the Muttrah City five. A view facing the sun has yaw = the survey's sun azimuth + 180°.
2. Atmosphere model (contract 6): the two layers and the fit at load as in the plan; the
   transmittance, multiple-scattering and sky-view tables at load and after a device reset, the
   aerial-perspective atlas per frame, all pixel passes; the atlas format checked for vertex
   texture fetch. Tables on the debug overlay; `renderer.atmosphere`.
   Built 2026-11-20 (`render(atmosphere)`, `ui(debug)`, `tools(atmosphere)`; shaders): the fit as
   decided with the owner (Earth's air on every level since 2026-11-20, a per-level sky gain applied
   as gain^`renderer.skyGainBlend`, default 0.5; [renderer.md](renderer.md#11-atmosphere-sky-ibl-water-reflections));
   the three load-time tables on the CPU (managed textures; pixel passes came out wrong, see the
   plan), the atlas a pixel pass with its march unrolled; A16B16G16R16F passes the vertex fetch check
   (filtered too). Checked by an agent on Muttrah City with `tools.atmosphere`: the tables match the
   CPU model the fit evaluates, the atlas runs from transmittance 1 to 0 at fog end with in-scatter
   0.25–0.47, fit and tables take about 250 ms at load; 12 device resets (`renderer.vsync`) and a
   blend change, no crash. The fit run natively on the survey's inputs for all four levels (gains
   2.7, 3.1, 5.1, 0.11 at night). Not yet seen by the owner (the Atmosphere tab); nothing on screen
   changes until task 3.
3. Sky and sun: the model draws the whole sky, clear (the painted dome and its cloud layers not
   shown; clouds are render milestone 12, owner 2026-11-20); the sun disc and its forward-scattering glow from the model, the flare's
   occlusion test kept. The sky is one full-screen pass at the far plane (`RaSky`, contract 7) in
   the dome's place, filling what nothing covers; under water it fills the water's fog colour, and
   thermals keep their heat gradient. The dome mesh draws only with `renderer.atmosphere 0`; at
   acceptance it and its techniques are deleted, and the sky moves to an effect of its own. Built:
   the sky and disc seen by the owner on Muttrah City on the dome (2026-11-20, "looks good",
   `renderer.skyGainBlend 0.5`); the full-screen pass replacing it, checked by an agent on Muttrah City: the sky fills what nothing covers, the disc and glow sit where retail's flare sprite does (`renderer.atmosphere 0` A/B), 6 device resets (`renderer.vsync`) without a crash; seen by the owner (2026-11-20, "looks good"); under water and thermals not checked (deferred by the owner, 2026-11-20). The
   banding in the dark blue and the sun's glare go to render milestone 13 (the tonemap's dither,
   bloom). Look for: no seam where terrain meets the sky; the horizon brighter and warmer
   toward the sun; the sky correct under water and in thermals.
4. Aerial perspective: `Ra_ApplyFog` replaced by one function (transmittance and in-scatter from the
   atlas by direction and distance) in the 18 effects that fog; particles per vertex, blending
   neighbouring slices by hand; the exponential haze curve, full haze at fog end, fog start unused.
   The two fog queues (`RSMTFogPass`, `RSMTFogTransparentPass`): deleted if every entry now fogs in
   its own shader, else kept with the reason recorded. Underwater fog and the thermal heat haze
   unchanged. Look for: ridges in layers, valleys hazier than ridgelines, haze warmer toward the sun.
   Built 2026-11-20 (`render(atmosphere)` 7eeb2da0 and 91dab088, contract 8; shaders): `Ra_GetAerial`
   reads the atlas on s9 per pixel (VPOS and distance; particles too, per pixel rather than per
   vertex: they already fog per pixel), with `RaAerial` in c223; retail fog under water, in first
   person and in thermals. Full haze is the sky's radiance along each ray (not the fog colour), so
   it meets the sky; the haze's shape is `renderer.hazeDepth`, `hazeFalloff`, `hazeSteepen`. In
   game on Muttrah City (`ridges`, A/B): no compile failures (load trace), frame rate normal;
   depth 2 greyed the near ridge, so 1 by default; steepening from 0.75 left a hazy wall
   at 500–700 m, from 0.35 the ridges separate into layers. Defaults to settle with the owner. The
   fog queues deleted (2026-11-20): nothing adds to slots 14 and 15 (the enqueue router fills 0–7,
   direct adds 8–11, 16–27 and 31, each shader its own queue), so their flushes drew nothing; loaded on Kashan Desert, the same image, and
   6 device resets (`renderer.vsync`) with the atlas drawing, no crash.
   Below the horizon the sky fills the horizon row, flat: from the air a bright horizon band over
   flat grey; task 5's geometry past the limit fills it (owner, 2026-11-20).
   Seen by the owner on Kashan Desert, 2026-11-20 ("looks good"): done, with the defaults as they
   are (`hazeDepth 1`, `hazeFalloff 400`, `hazeSteepen 0.75`; 0.35 remains the alternative).
5. View distance, as in the plan: geometry cut off at the radial distance (the far plane pushed
   past it), the last 10–15 % faded by alpha-to-coverage dither; objects faded to the radiance
   behind them; the low-detail surround terrain and a far ground ring (or sea) drawn past the
   limit under aerial perspective, without vehicles, objects or vegetation. Each level's view
   distance unchanged. Look for: no reach at the screen's edges past its centre; aircraft
   against the sky gone at the same distance as vehicles against terrain; from a jet, a
   landscape to the horizon instead of a fogged void; nothing pops at the limit.
   Decided with the owner, 2026-11-20: the haze is no longer steepened to full at fog end
   (`renderer.hazeSteepen` deleted); it stays exponential and carries on past fog end, and
   objects end by the dither ([renderer.md](renderer.md#11-atmosphere-sky-ibl-water-reflections)).
   Built 2026-11-20 (`render(atmosphere)`, contract 9; shaders), A/B `renderer.viewDistance`:
   the cut (`Ra_ViewDistanceClip`, a 4x4 Bayer dither over the last 15 % by distance from the
   camera, in every opaque world effect and its prepass; particles, trails, decals and blended
   roads fade on alpha); the world view's projection unbounded (reversed float depth), its
   frustum still at the view distance, so objects cull as before; with the cut on, the terrain
   culler and the surrounding ring take the frustum out to 40 km; past fog end the haze carries
   on at the last slice's mean extinction along the ray. In game on Muttrah City (`ridges`,
   `high`): objects end at the limit, the bay, coast and ranges go on behind in layers of
   haze; no shader error. Not yet seen by the owner. Left: water past the limit (the sea out to
   the horizon) and the far ground ring; the dither's look (4x4 Bayer per pixel, alpha to
   coverage not used yet); jets and anti-air; device resets; the cost of terrain to 40 km.
6. Light shafts: the atlas's in-scatter shadowed per slice by the cascades;
   `renderer.lightShafts`. Look for: shafts in the street view and through trees. Dropped at
   acceptance if they do not pay for their cost.
7. Sky cube: at load and after a device reset, the dome and atmosphere above the horizon, the
   ground term below it, prefiltered GGX one mip per roughness; its irradiance as 9 spherical-harmonic
   coefficients; the split-sum BRDF table (or the analytic fit if task 1 finds no third slot); the
   atlas and cube on reserved samplers. Retail environment maps it replaces are deleted at
   acceptance where task 1 finds no other reader.
8. Shared lighting model: direct and ambient specular from one roughness and F0 (GGX, split sum),
   with an occlusion input; sky light as the irradiance at the normal times occlusion, scaled to the
   authored sky colour's luminance; gloss to roughness; the material class table (F0, roughness
   range; the PR mapping outside this tree); specular anti-aliasing (roughness widened by the
   normal's screen-space derivatives, Toksvig from the mipped normal's length). `renderer.skyIbl`.
9. Bundled meshes (judged on test_bootcamp, which holds most vehicles): vehicles, cockpits and first-person models; the env map leaves albedo and becomes
   specular; class per material; occlusion by the hemi map, first-person models by the sky
   visibility under the camera (the terrain lightmap's sky channel copied to memory at load, read
   under the camera, smoothed over about 0.3 s). Look for: paint and glass reflecting the sky,
   strongest at grazing angles; vehicles lit from above and darker beneath; no edge sparkle.
10. Static meshes: gloss map, occluded by the lightmap's sky visibility. Look for: glass and metal on
   buildings reflecting the sky; walls lit by the sky from its direction.
11. Soldiers (hemi map), decals and terrain.
12. Water: the scene resolved to a texture just before water, after the sky, the fog pass, decals
    and the particles drawn before water (also the refraction source; depth is s15); a screen-space
    march against the linear depth, falling back to the prefiltered sky; replaces water's
    `CubeMap`; none from underwater. `renderer.waterSsr`. Look for: buildings and shore reflected.
13. Acceptance: the owner's look at the reference views on the three maps against milestone 10;
    the night layer loads and looks no worse; benches against milestone 10 (8.39–8.48, 15.34,
    5.22 ms) within the plan's 1 ms budget or the regression accepted by the owner; GPU pass
    times; old paths and variables deleted; as-built moved to renderer-pipeline.md.

Milestone 10 (terrain in one pass) accepted by the owner, 2026-11-20 (benches on the three maps and
a visual check in game); the old terrain passes, `renderer.terrainOnePass`, the accumulation
target's use, the culler's detail distance and `terrainCuller.lodSwitchDistance` deleted. As built,
with its frame-time change: [renderer-pipeline.md](renderer-pipeline.md#terrain). The deletion
loaded by an agent on all three maps, 2026-11-20: Muttrah City views `mountain` (the same image as
task 6's one-pass shot) and `roadbug` (wind and rooftops only), Fallujah West `city1`, Saaremaa
`runway`; `bench1`/`bench` 8.39–8.48, 15.34 and 5.22 ms, no crash, no shader error.

Open from milestones 8 and 9:
- Crash in the first frame after a device reset: fixed in `717b2671` (a stale RaShadowViewProj
  handle cached by effect pointer). Tried by an agent, Muttrah City, 2026-11-19: 24 resets
  (`renderer.vsync` toggled at a view with the sun cascades drawing) and one `bench.run`, no
  crash; before, about one reset in four crashed. Not yet seen by the owner.
- Sun shadows: far overgrowth cells cast (`980b32aa`) and the blend to baked goes by camera
  distance (shaders); loaded on Saaremaa, not yet seen.
- The default video profile picks 8x antialiasing (log) while the scene runs 4x. Settle with fixed
  4x (a single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Road overlays (road static meshes) drawn over buildings at range: fixed 2026-11-20 (`51c0b38e`
  and shaders), roads pull 0.1 % of their distance toward the camera instead of a depth bias; seen
  fixed by an agent at the owner's view `roadbug` (Muttrah City); seen fixed by the owner.
- Cached static casters (owner, 2026-11-19, "most of the map is static"): the far cascades' static
  casters kept in their map while the snapped cascade holds still, moving objects drawn over a
  copy each frame. Not planned yet.

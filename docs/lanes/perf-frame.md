# Frame time

The `perf` lane ([AGENTS.md](../../AGENTS.md)) for the running game: make frames cheaper without
changing what is drawn. Load times are in [perf-load.md](perf-load.md).

Work in renderer code that is this fork's own (the sun cascades, the depth prepass) is `render`
work even when its purpose is speed; this document records its cost alongside, so the frame is
seen as a whole.

## Measuring

- **Who is the bottleneck.** Per-thread CPU use of the game (`top -H` on the BF2.exe process): a
  main thread at 100 % while the GPU spans leave idle time means the frame is CPU-bound. Check
  `game.lockFps` first; a frame cap looks the same.
- **GPU per pass.** `BF2_GPU_TIMERS` ([dev.md](dev.md)). Spans run from mark to mark, so when
  the CPU is the bottleneck the GPU's idle time lands in the span that waits (usually `hud`, which
  runs to Present).
- **CPU per function.** Sample the main thread with Linux `perf record -t <tid>` while the game
  runs under Wine. perf cannot name the PE images, so the sampled addresses are resolved by hand:
  module bases from `/proc/<pid>/maps` (each image's header page is mapped from its file), sizes
  from the PE headers, symbols from the link maps (`BF2.map`, `RendDX9.map`). A link map lists public
  symbols only, so samples in static or inlined code are credited to the nearest public symbol
  before them: object-file totals are reliable, single function names approximate.
- **Draws.** `BF2_DRAW_STATS` ([dev.md](dev.md)) for what each pass issues. It costs about
  9 % of the main thread itself (its lookups), so CPU samples are taken in a session without it.
- **Sun cascades' whole cost.** `renderer.sunShadows 0` at the same view: the frame-time difference
  is the caster pass, CPU and GPU together.

## Baseline

Before render milestone 8. Muttrah City 2, `gpm_skirmish` 16, the same spot in the city, 4x MSAA,
sun cascades on, 2026-11-18, engine `fd85ae94` (plugins with `/O2`):

- **6.6 ms a frame (153 fps)**, `game.lockFps` 200. The main thread is still at 100 %, DXVK's
  submission thread at 23 %: still CPU-bound.
- GPU: about 3.5 ms of pass work (sun cascades 1.4, depth prepass 0.8, world opaque 0.9, sun mask
  0.1).

Main thread by image, 10 s of samples:

| Share | Image |
|---|---|
| 46 % | RendDX9.dll |
| 18 % | BF2.exe: object manager culling and cull fade, game logic |
| 15 % | d3dx9_25.dll: the D3DX effect framework, called per draw (parameters, passes, commits) |
| 13 % | d3d9.dll (DXVK) |
| 6 % | C runtime |
| < 1 % each | BF2Audio, Python, SwiffPlayer |

Main thread by subsystem (RendDX9 and BF2.exe): object culling 17 %, mesh draws and parameter apply
14 %, sun caster pass 7 %, state manager 5 %, render queues 3 %, `tailField` 0.8 %.

## After render milestone 8

Milestone 8's LOD policy (full-detail terrain, LOD 0 bundled meshes, static switch distances x3, no
distance cull) adds draws: the owner sees about 20 % lower frame rate on Muttrah City. Still
CPU-bound: the GPU spans sum to about 5.7 of 12.6 ms on Muttrah City and 4.9 of 8.25 ms on
Saaremaa; the rest is the `hud` span, the wait in Present.

Saaremaa, overview camera over forest and buildings, 1600x900, 2026-11-19: depth prepass 1.47 ms,
world opaque 1.22, sun cascades 1.53, resolve 0.19, depth rebuild 0.05.

GPU-side lead: `Ra_LodDitherClip` runs in every lit shader, so opaque shaders lose early depth
rejection even at dither (0, 1); a static branch or a separate dithered technique would avoid it.

## Render milestone 9, task 1

Fixed views, 1600x900, 4x MSAA, vsync off, `game.lockFps 1000`, engine `aa754216`, 2026-11-19.
Muttrah City 2 `gpm_skirmish` 16, view `high` (overview over the city); Fallujah West `gpm_cq` 64,
view `city1`. Main thread at 100 % and the `hud` span (the wait in Present) large in every case:
CPU-bound. Times are per frame; CPU shares of the main thread are turned into ms of the frame.

| | Muttrah `high` | Fallujah `city1` |
|---|---|---|
| Frame, cascades on | 14.9 ms | 28.9 ms |
| Frame, `renderer.sunShadows 0` | 8.7 ms | 15.6 ms |
| Caster pass, whole cost | 6.2 ms (42 %) | 13.3 ms (46 %) |
| Draws: cascade c0 / c1 / c2 / c3 | 0 / 97 / 1232 / 3939 | 31 / 228 / 2437 / 7181 |
| Draws: depth prepass | 2507 | 4524 |
| Draws: lit and other passes | 2770 | 4623 |
| Draws, total | 10545 | 19024 |

Draws are the last frame of a `BF2_DRAW_STATS` session at the same view. Nearly all of them are
static mesh materials, one draw per material in every pass (the caster and prepass techniques
included); terrain, overgrowth and roads are a few hundred. Cascade c3 (about 107 to 400 m) holds
three quarters of the caster draws at about 185 triangles a draw; on Fallujah the fog hides most
of what it draws.

Main thread by image and object file, ms per frame, cascades on / off / difference:

| | Muttrah on | off | diff | Fallujah on | off | diff |
|---|---|---|---|---|---|---|
| RendDX9.dll | 8.06 | 4.27 | 3.79 | 15.62 | 8.04 | 7.58 |
| d3dx9_25.dll (effect framework) | 2.44 | 1.46 | 0.98 | 5.32 | 2.58 | 2.75 |
| d3d9.dll (DXVK) | 2.05 | 1.41 | 0.64 | 3.62 | 2.42 | 1.20 |
| BF2.exe | 1.29 | 0.90 | 0.39 | 2.60 | 1.51 | 1.09 |
| StateManager | 1.83 | 1.00 | 0.83 | 3.30 | 1.72 | 1.58 |
| RenderQueues | 1.58 | 1.04 | 0.54 | 2.69 | 1.76 | 0.93 |
| RaShader | 1.22 | 0.63 | 0.59 | 2.49 | 1.37 | 1.11 |
| StaticMesh | 1.07 | 0.48 | 0.59 | 2.40 | 1.03 | 1.37 |
| ObjectDrawer | 0.77 | 0.23 | 0.54 | 2.08 | 0.64 | 1.44 |

Top functions (nearest public symbol, Muttrah, cascades on): StateManager's pre-draw apply 8.4 %,
`RaShader::applyParameterVector` 5.6 %, `StaticMesh::drawVanilla` 4.1 %, render queue walk 2.5 %,
`renderQueuesDrawShadowCasters` 2.4 %, `StaticMeshMaterial::applyStreams` 2.3 %, box-frustum tests
1.5 %, `ObjectDrawer::submitShadowCasters` 1.3 %.

Read:
- The caster pass is the largest single cost, and most of it is per draw: D3DX, the state manager,
  Ra parameters, mesh draws and DXVK. Its own culling (ObjectDrawer, the BF2.exe cullers, the caster
  sort) is about 1 ms on Muttrah and 2.5 ms on Fallujah.
- The main thread spends about 1.4 µs (Muttrah) to 1.5 µs (Fallujah) per draw, culling included.
- Leads for the task order: draws in the depth-only passes (prepass, casters) could be one per mesh
  rather than one per material where the materials share the depth shader (no alpha test); and
  contribution culling applied to casters by their size in cascade texels would remove much of
  cascade c3's small draws.

## Render milestone 9, task 5

Main thread at Muttrah `high`, 10 s samples, engine with task 5, 2026-11-19. Shares of the main
thread, programs (`renderer.submit 1`) / D3DX (`0`), both with the banded depth order:

| | Programs | D3DX |
|---|---|---|
| d3dx9_25.dll | 5.5 % | 15.7 % |
| RaShader.cpp | 2.3 % | 6.8 % |
| Submit.cpp | 12.6 % | — |
| StateManager.cpp (pre-draw apply 10.1 / 9.7) | 14.0 % | 12.8 % |
| d3d9.dll (DXVK) | 12.8 % | 13.0 % |

Submit.cpp's largest parts: the holder calls into the sink (`applyUsed` 3.8 %), matrix and vector
conversion (`setMatrices` 2.5, `setVectors` 1.6), register writes (`emitTarget` 1.6). The first
version (strict near-to-far order, a conversion per component) spent 16.9 %: the order switched
program at almost every prepass draw, each switch binding the program's states, defaults and global
parameters again. `bench1`: 12.44 ms programs, 12.92 ms D3DX; before the banded order 13.46 / 13.52.

Then float values copied whole into the sink and register-shaped targets (row-packed matrices,
vectors) uploaded in place, without the per-component conversion and gather. Same-session pairs,
3 runs each, skirmish layer:

| Path | Programs | D3DX | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 12.03 ms | 13.07 ms | 8.0 % |
| Fallujah West `bench` | 21.52 ms | 23.43 ms | 8.2 % |

Fallujah West has the most draws (16085 a frame), so the per-draw work shows most there; Muttrah
City's runs spread less (about 2 % against 3 %), so it separates small changes better.

Then the pre-draw apply: two thirds of its samples were the pixel constant flush scanning one dirty
byte per register from the material constants (c0 up) to the LOD dither (c222), about 210 clean
bytes a draw. With the dirty registers as a bit mask (`f3515fb4`) the pre-draw apply and the flush
take 2.5 % of the main thread, from 8.2 % (Muttrah `high`, programs, 10 s samples). Both paths
go through the state manager, so both gain. Same-session pairs, 3 runs each, skirmish layer:

| Path | Programs | D3DX | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 10.86 ms (12.03 before) | 11.73 ms (13.07) | 7.4 % |
| Fallujah West `bench` | 19.40 ms (21.52) | 20.78 ms (23.43) | 6.6 % |

Then the coming draws' parameter blocks prefetched (`85cf4ae4`: the entry eight draws ahead, its
object and material blocks four ahead) and the depth order sorted on keys read once per entry.
At Muttrah `high` the holder calls into the sink go from 5.4 % to 3.0 % of the main thread, but the
prefetch itself takes 1.1 %: the stall moves more than it goes. The depth sort goes from 2.5 % to
0.9 %. `bench1`, 3 runs each: 10.90 ms with programs (10.86 before, spread 2.4 %), 11.57 ms through
D3DX (11.73): within the runs' spread.

At the same view, with `BF2_DRAW_STATS`: the depth passes (prepass and cascades) draw 7390 entries
without alpha test a frame, in 5069 (mesh instance, effect) groups; one draw per mesh would remove
at most 2321 of the frame's 10541 draws.

The prefetch was then dropped (`f6dd0829`). One draw per mesh in the depth passes (`7bdd3a77`): at
Muttrah `high` the frame's draws go from 10541 to 8834 (prepass 2507 to 1969, cascade c3 3939 to
3098), the same primitives. The CPU moves more than it goes: Submit.cpp 11.0 to 8.6 % of the main
thread and the state manager 7.2 to 6.5 %, but the walks with their run test 11.6 to 13.9 % and
the materials' stream records 2.3 to 3.0 %; DXVK stays at 14.8 %, so its cost here is not per
draw. Same-session pairs, 3 runs each, programs against D3DX:

| Path | Programs | D3DX |
|---|---|---|
| Muttrah City `bench1` | 10.94 ms (10.90 before) | 11.90 ms (11.57) |
| Fallujah West `bench` | 18.70 ms (19.40 before the keyed sort) | 20.52 ms (20.78) |

D3DX draws in the instance-first order without merging, which costs it material switches on
Muttrah. Fallujah West, with the most draws, gains 3.6 %, near the runs' spread (2.9 %).

## Render milestone 9, task 4

Caster culling (`renderer.casterCull`): each object from the light volume's grid query is tested
against the whole shadow range swept toward the sun before its visual update, geometry update and
submission ([Sun shadows](render.md#sun-shadows)). Caster draws per cascade are equal
with the variable at 1 and 0 at every view counted (`BF2_DRAW_STATS`, last frame):

| View | Captured casters, 0 / 1 | Cascade draws c0–c3 |
|---|---|---|
| Fallujah West `city1` | 10515 / 7020 | 26, 193, 2175, 6469 |
| Fallujah West `city2` | 8308 / 4218 | 38, 369, 1073, 3586 |
| Fallujah West `city3` | 10471 / 6617 | 39, 217, 1432, 6024 |
| Muttrah City `high` | 4908 / 3770 | 0, 51, 948, 3097 |
| Muttrah City `market` | 4934 / 2130 | 44, 181, 644, 1502 |

A first version tested only the objects' visual bounding spheres and lost 6 of the 6476 c3 draws
at `city1`: the casters' own spheres (then the sphere around the transformed box's world-aligned
box) were 1.1 to 1.3 times larger and centred up to a metre off the object's position. The casters'
spheres are now the local box's own sphere, which also takes 20 draws out of c1–c3 at `city1`,
and the range test keeps an object when either sphere touches.

At Fallujah `city1` (10 s samples) `ObjectDrawer::submitShadowCasters` goes from 4.5 % to 2.2 % of
the main thread, plus 1.6 % for the range test. The two pointer sorts of the gather (3.1 % before
the task) are gone in both settings: the dedupe and the camera sets' membership test use a hashed
pointer set. Same-session pairs, 3 runs each, skirmish layer, programs, `casterCull` 1 against 0:

| Path | 1 | 0 | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 9.51, 9.67 ms | 10.91, 10.54 ms | 10.6 % |
| Fallujah West `bench` | 18.17, 17.52 ms | 19.65, 19.59 ms | 9.0 % |

View shots, `casterCull` 1 against 0: Muttrah `high` 19 pixels differ (17 between two shots at
1); Fallujah `city1` 5414 (3680 between two shots at 1), the same regions in both: smoke and
wind-blown palms.

## Render milestone 9, task 6

The queue walks (`drawWalkDefault`, `drawWalkNear`) draw through programs: static, bundled and
skinned meshes, roads, leaves and water in their lit passes. No technique the three maps reached
was refused (no `Submit: ... not drawable` line). Same-session pairs, 3 runs each, skirmish
layer, `renderer.submit` 1 against 0 (0 now also puts the depth passes back on D3DX):

| Path | 1 | 0 | Gain | Task 5's pair |
|---|---|---|---|---|
| Muttrah City `bench1` | 10.53, 10.46 ms | 11.78 ms | 11.2 % | 10.94 / 11.90 ms |
| Fallujah West `bench` | 18.39 ms | 20.26 ms | 9.2 % | 18.70 / 20.52 ms |
| Saaremaa `bench1` | 8.27 ms | 8.70 ms | 4.9 % | not measured |

View shots, 1 against 0: Muttrah `high` 19 and `market` 580 pixels of 1.44 M (wind-blown palms);
Fallujah `city1` 8072, `city2` 491, `city3` 15273 (drifting smoke and palms only); Saaremaa
`forest1` 2242, `forest2` 8323 (7957 between two shots at 1: wind in the foliage), `runway` 3028.

## Render milestone 9, task 7

Terrain and undergrowth through effect programs. 1920x1080 from here on (earlier sections are
1600x900), 4x MSAA. Same-session pairs, 3 runs each, `renderer.submit` 1 against 0 (0 puts every
pass back on D3DX), engine `965bb8bf`, 2026-11-19:

| Path | 1 | 0 | Gain |
|---|---|---|---|
| Muttrah City `bench1` | 9.53 ms, 1 % low 13.04 | 10.69 ms, 14.44 | 10.9 % |
| Fallujah West `bench` | 18.34 ms, 25.59 | 21.42 ms, 31.85 | 14.4 % |
| Saaremaa `bench1` | 6.71 ms, 10.64 | 7.09 ms, 20.86 | 5.4 % |

Saaremaa ran while the host built packages: its pair is not clean (spread between runs at 0:
7.1 % average, 105 % 1 % low). Terrain is about 330 draws a frame and undergrowth about 10, so
most of each gain is task 5 and 6's; this task moves the remaining per-draw D3DX calls of these two
effects out of the frame.


## Render milestone 9, task 8

What instancing could leave, before it was built: `BF2_DRAW_STATS` at fixed views, engine
`dev(stats)` commits of 2026-11-19, 1920x1080. "Depth runs" are the depth passes' draws after one
draw per mesh; "groups" the draws left were every run of one effect, material and run length one
instanced draw (clip-planed runs stay single). "Lit" is the static and overgrowth queue walks
through programs, grouped by effect, technique and material: an upper bound, as instance
lightmaps and point lights are not compared.

| View | Prepass runs / groups | Cascades c0–c3 runs / groups | Lit draws / groups | Frame draws |
|---|---|---|---|---|
| Muttrah `high` | 1954 / 539 | 3865 / 994 | 2646 / 745 | 8831 |
| Muttrah `market` | 2387 / 598 | 2202 / 841 | 3124 / 846 | 8068 |
| Fallujah `city1` | 4059 / 688 | 8630 / 1377 | 4482 / 852 | 17515 |
| Fallujah `city3` | 4782 / 667 | 7491 / 1243 | 5257 / 828 | 17886 |
| Saaremaa `forest1` | 1512 / 266 | 1680 / 258 | 1654 / 321 | 4984 |
| Saaremaa `runway` | 1268 / 277 | 1379 / 295 | 1385 / 324 | 4174 |

Built (static meshes on contract 3; overgrowth trunks and leaves not yet), frame draws at the same
views: Muttrah `high` 3723, `market` 3509; Fallujah `city1` 4980, `city2` 3605, `city3` 4250;
Saaremaa `forest1` 4179, `forest2` 5448, `runway` 3238. Same-session pairs, 3 runs each,
`renderer.instancing` 1 against 0, engine of 2026-11-19 20:15:

| Path | 1 | 0 | Gain | Task 7 (`submit 1`) |
|---|---|---|---|---|
| Muttrah City `bench1` | 8.94 ms, 1 % low 11.91, 3354 draws | 10.45 ms, 13.79, 7697 draws | 14.4 % | 9.53 ms |
| Fallujah West `bench` | 15.99 ms, 21.42, 4447 draws | 19.08 ms, 26.40, 14806 draws | 16.2 % | 18.34 ms |
| Saaremaa `bench1` | 6.05 ms, 8.50, 3548 draws | 6.45 ms, 9.37, 4312 draws | 6.2 % | 6.71 ms |

At 0 every instance still goes through the stream and its record, so 0 is slower than task 7's
path; against task 7, 6 to 13 %. The grouping first cost about 15 % of the main thread at Fallujah
`city1` (each entry's parameters captured twice through the holders); with groups joined on the
hash and records read in place, the submission layer is 13.5 % of it, the instance hash 3.3 % and
the depth runs' sort about 3.5 % (`runs/perf/t8b-city1-i1`).

Near trees after it (`leaf`, `trunkstmdetail` and `base` instanced, prshaders `d6db632`): at
Saaremaa `forest1` they were about 2700 of 4157 draws (`leaf` 530 prepass, 530 lit, 867 in the
cascades; `trunkstmdetail` 228, 228, 477). After: 2132 draws (prepass 829, lit and other 1005,
cascades 298); `leaf` keeps 301 groups in the prepass and the lit pass, one per tree template's
LOD material. Shots against task 8's: Saaremaa `forest1` 224, Muttrah `high` 23, `market` 133
pixels of 2.07 M (3 % fuzz); Fallujah `city1` 5057, `city3` 32517, all on smoke. Shader files
swapped in a running game do not reload (no `.cfx` written), so the pairs are separate launches,
3 runs each:

| Path | Old shaders | New |
|---|---|---|
| Saaremaa `bench1` | 6.50 ms, 1 % low 9.82 | 5.96 ms, 9.35 |
| Fallujah West `bench` | 16.48 ms, 21.09 | 16.30 ms, 20.86 |
| Muttrah City `bench1` | 8.82 ms, 12.11 | 8.81 ms, 12.47 |

Then the grouping without a sort and the instance key read in place (`3c745649`, `67d04f51`;
[Instancing](render.md#instancing)). Main thread at Fallujah `city1`
(`runs/perf/t8e-city1`): the depth runs' sort (3.5 %) gone, `drawDepthPassSubmit` 4.6 to 3.4 %;
the hash 3.3 to 3.0 %, so its cost is reading each entry's object and lighting blocks, not the
sink calls it no longer makes. Shot `city1` against `t8-city1`: 8792 pixels of 2.07 M (3 % fuzz),
all on smoke and wind-blown palms; 4886 draws (4980). Separate launches against the near-trees
column above, 3 runs each:

| Path | Before | After | Spread (average / 1 % low) |
|---|---|---|---|
| Fallujah West `bench` | 16.30 ms, 1 % low 20.86 | 14.79 ms, 18.68 | 0.14 / 1.53 % |
| Muttrah City `bench1` | 8.81 ms, 12.47 | 7.85 ms, 11.02 | 3.23 / 7.39 % |
| Saaremaa `bench1` | 5.96 ms, 9.35 | 5.22 ms, 8.03 | 0.52 / 5.34 % |

The gain is larger than the profile's share (about 2 % of the main thread); part of it may be
the host between launches. A same-session pair needs a switch this change does not have.

Parked on 2026-11-18, when 153 fps left headroom and the renderer roadmap went first. Resume here
after render milestone 9, which takes items 1 and 3.

1. Measure what the sun cascades cost the CPU in total (a run with them off) before redesigning
   the caster submission, which repeats the culling over the cascades' light volume.
2. Object culling (17 %): the object manager's grid and cull fade, the object drawer's box tests.
3. Per-draw cost: D3DX effect calls (15 %) and Ra parameter apply. Replacing D3DX's per-draw
   parameter and pass handling with the renderer's own constant uploads is `render` work: render
   milestone 9 ([Submission](render.md#submission)), which also takes the sun caster
   culling of item 1.

Open:
- An intermittent stack overflow while loading, seen once with the unoptimised plugins
  (`virtual_setup_exception stack overflow`, address at RendDX9 offset `0x821d` of engine `b3342099`).
  Rebuild that commit's `RendDX9.map` to name the function; with `/O2` it may have moved or gone. If
  it returns, `WINEDEBUG=+seh` with the module base pins it.

## Render milestone 9, task 9

Contribution culling ([render.md](render.md#contribution-culling)), engine
`2002788d` plus the default of 8, 1920x1080, an agent, 2026-11-19. `BF2_DRAW_STATS` at Muttrah
`high` (threshold 3 and 8 mixed in one session): about 100 static meshes culled and 20 fading a
frame. Benchmarks, `renderer.contributionCull` changed in one session, 2 runs each:

| Map, path | 0 | 3 | 8 |
|---|---|---|---|
| Muttrah City `bench1` | 8.08, 8.08 ms | 7.95, 8.08 ms | 7.51, 7.20 ms |
| Fallujah West `bench` | 14.52, 14.67 ms | — | 13.21 ms |
| Saaremaa `bench1` | 5.11, 5.13 ms | — | 5.06, 5.11 ms |

Saaremaa gains little: its small objects are overgrowth, which keeps its own fade.

## State

Seen in game on Muttrah:
- The plugin DLLs compile with `/O2` (`BF2_PLUGIN_OPTIMIZE`): 12.2 ms to 6.6 ms a frame
  (13.3 ms at the baseline before it); RendDX9, SwiffPlayer and BF2Audio had been built without optimisation.
- `Renderer::tailField` answers from a table indexed by retail offset: 5.6 % to 0.8 % of the main
  thread (measured before `/O2`).
- The sun casters carry their sort keys (shader, material), copied at capture. No measurable gain:
  the sort's cost had been the unoptimised insertion sort, not the pointer chasing.

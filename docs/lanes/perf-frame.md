# Frame time

The `perf` lane ([AGENTS.md](../../AGENTS.md)) for the running game: make frames cheaper without
changing what is drawn. Load times are in [perf-load.md](perf-load.md).

Work in renderer code that is this project's own (the sun cascades, the depth prepass, draw
submission) is `render` work even when its purpose is speed; this document records its cost
alongside, so the frame is seen as a whole. The per-task measurements of render milestone 9 are in
its commit messages (`git log --grep 'milestone 9'`).

## Measuring

- **Who is the bottleneck.** Per-thread CPU use of the game (`top -H` on the BF2.exe process): a
  main thread at 100 % while the GPU spans leave idle time means the frame is CPU-bound. Check
  `game.lockFps` first; a frame cap looks the same.
- **Benchmarks.** `bench.run` on the paths of [tools.md](tools.md#3-benchmark-path): Muttrah City
  `bench1`, Fallujah West `bench`, Saaremaa `bench1`. Compare same-session pairs (3 runs each,
  the change's switch flipped), not separate launches: the host drifts between launches. Fallujah
  West has the most draws, so per-draw work shows most there; Muttrah City spreads least (about
  2 %), so it separates small changes best. Shader files swapped in a running game do not reload
  their `.cfx`, so a shader change needs separate launches.
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

## Current

1920x1080, 4x MSAA, after render milestone 11 (2026-11-22): Muttrah City `bench1` 9.20–9.26 ms,
Fallujah West `bench` 16.00 ms, Saaremaa `bench1` 5.78 ms. Every frame is CPU-bound. Milestone 11
added 0.65–1.45 ms, most of it the scenery drawn past the view distance (its section in
[atmosphere.md](../render/atmosphere.md#11-atmosphere-sky-ibl-water-reflections)); accepted by the owner. After
milestone 10 (2026-11-20): 8.39–8.48, 15.34 and 5.22 ms.

## Render milestone 9 (draw submission)

Before it (1600x900, engine `aa754216`, 2026-11-19), the frame was CPU-bound on per-draw cost: about
1.4 µs of main thread per draw, culling included, 10–19 k draws a frame, nearly all static mesh
materials, one draw per material in every pass. The sun caster pass was 42 % (Muttrah `high`) to
46 % (Fallujah `city1`) of the frame, cascade c3 three quarters of its draws. D3DX's effect
framework, the state manager and Ra parameters were the largest per-draw shares.

What each task gained on the benchmark paths, same-session pairs against its switch:

| Task | Change | Muttrah `bench1` | Fallujah `bench` | Saaremaa `bench1` |
|---|---|---|---|---|
| 5 | Effect programs for the depth passes; register-shaped uploads; dirty-register mask (`f3515fb4`) | 13.07 → 10.86 ms | 23.43 → 19.40 ms | — |
| 4 | Caster culling against the swept shadow range (`renderer.casterCull`) | 10.6 % | 9.0 % | — |
| 6 | Queue walks through programs | 11.2 % | 9.2 % | 4.9 % |
| 7 | Terrain and undergrowth through programs (1920x1080 from here) | 10.9 % | 14.4 % | 5.4 % |
| 8 | Instancing of static meshes, then near trees, then grouping without a sort | 8.94 → 7.85 ms | 15.99 → 14.79 ms | 6.05 → 5.22 ms |
| 9 | Contribution culling, threshold 8 | 8.08 → 7.36 ms | 14.60 → 13.21 ms | 5.12 → 5.08 ms |

Lessons for later work:
- DXVK's share stayed at about 15 % of the main thread when draws fell by a sixth: its cost here
  is not per draw.
- Prefetching the coming draws' parameter blocks moved the stall rather than removing it
  (dropped, `f6dd0829`).
- Saaremaa gains least from per-draw work: its small objects are overgrowth, which keeps its own
  fade.
- Caster culling first lost draws: the casters' spheres must be the local box's own sphere, and
  an object stays when either sphere touches the range.
- Remaining GPU lead: `Ra_LodDitherClip` costs early depth rejection in every lit shader
  ([README.md](../render/README.md#8-anti-aliasing)).

## State

Seen in game on Muttrah City:
- The plugin DLLs compile with `/O2` (`BF2_PLUGIN_OPTIMIZE`): 12.2 ms to 6.6 ms a frame
  (1600x900); RendDX9, SwiffPlayer and BF2Audio had been built without optimisation.
- `Renderer::tailField` answers from a table indexed by retail offset: 5.6 % to 0.8 % of the main
  thread (measured before `/O2`).
- The sun casters carry their sort keys (shader, material), copied at capture. No measurable gain:
  the sort's cost had been the unoptimised insertion sort, not the pointer chasing.

Parked (2026-11-18). Render milestone 9 took the per-draw cost and the caster culling; left for the
lane: object culling (the object manager's grid and cull fade, the object drawer's box tests; 17 %
of the main thread before milestone 9, not re-measured).

Open:
- An intermittent stack overflow while loading, seen once with the unoptimised plugins
  (`virtual_setup_exception stack overflow`, address at RendDX9 offset `0x821d` of engine `b3342099`).
  Rebuild that commit's `RendDX9.map` to name the function; with `/O2` it may have moved or gone. If
  it returns, `WINEDEBUG=+seh` with the module base pins it.

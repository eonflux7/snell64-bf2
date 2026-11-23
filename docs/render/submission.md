# Renderer: draw submission and shader pipeline

Part of [the renderer as built](README.md).

## Submission

Render milestone 9 (accepted by the owner, 2026-11-20). Frame time on the benchmark paths against
the D3DX path it replaced (same build and session, 1920x1080 4x, 3 runs each): Muttrah City
`bench1` 11.36 to 7.93 ms (draws 7713 to 3383), Fallujah West `bench` 20.66 to 14.11 ms (14848 to
4418), Saaremaa `bench1` 6.24 to 5.04 ms (4318 to 1644); view shots matched but for wind, smoke and
water. The depth prepass, water's linear depth, the sun casters and the render queues' walks (every lit mesh pass:
static, bundled and skinned meshes, roads, leaves, water, first person) draw through `Submit.cpp`
instead of the D3DX effect calls. A **program** is the one pass of one technique of a Ra effect,
built the first time a walk reaches it and again when the effect is rebuilt or reset; a technique
of more than one pass gets none and its walk stays on D3DX. A queue entry's technique key 0, or a
key its effect lacks, takes the effect's default technique, as `RaShader::begin`. Ra effects share
no effect pool, so every value a program reads comes from a holder or the effect's own default:

- Its shaders and fixed states, recorded from one `BeginPass` through a recording state manager.
- Where each parameter its shaders read lives: from the vertex and pixel shaders' constant tables
  (float, int and bool registers, struct members, arrays, either matrix packing), and the sampler
  stages each texture parameter feeds (each texture parameter holds a distinct marker texture while
  the pass is recorded).
- Which of the material states (contract 2) follow their parameter: the pass is recorded again with
  the parameter changed, and only a state that changes with it is set per draw.
- The Ra holders it reads, per set (instance, global, template) and block. The other parameters keep
  the effect's values, uploaded when a run of draws begins.

Drawing a run: a state-manager bracket, the program's shaders and states, its effect-wide (global)
parameters. Each draw: the material's streams, the template and instance parameters, the clip
planes, the material states, the LOD dither, then `RaShader::draw` (pre-draw apply and the indexed
draw). The holders apply into a sink with the `ID3DXEffect` shape that turns each value into
register writes in the state manager's mirror, so every holder kind works unchanged and D3DX is not
called while drawing. Constants in the engine's pixel registers (c200 on) keep the engine's values,
as through D3DX.

One draw per mesh: a static mesh's index buffer holds whole-template vertex indices (each
material's indices plus its vertex start, 32-bit past 65536 vertices), and every static material
draws with base vertex 0, so a LOD's materials lie in one index range. The depth walks order the
entries by effect, then mesh instance, then material, and draw a run of the same instance's
consecutive materials as one range when none is alpha tested and they share the LOD dither, the
declaration and the vertex buffers (`depthRunLength`). The run's later materials upload none of
their parameters: the depth techniques read the material only for alpha.

Effect programs: terrain (`terrainshader.fx`: `RaTerrain`, prepass, caster and surrounding passes)
and undergrowth (`undergrowth.fx`) draw through the engine's `Effect` wrapper, which they opt in
(`effectEnableSubmit`). An opted-in effect's `beginPass` binds the program of the current
technique's pass (any pass of a technique of several), built on first use as above with the same
recording, and with two checks a Ra program does not need: every constant register D3DX uploads at
`BeginPass` must lie in a parameter's targets (a preshader's output would not), and the pass's fixed
states must not change when every numeric parameter changes. A pass failing either draws through
D3DX, begun lazily for it. The effect's parameter writes
(`FXParameter`) go to a value store in the submission layer instead of D3DX: each entry holds the
parameter's value (a reference on its texture), read from D3DX when first written, and a write
during a program's pass is uploaded straight into that program's registers and samplers. The store
is written to D3DX before a D3DX pass (during which writes go to both), before the effect is rebound
(its values carried to the reloaded effect) and on device loss, where it is dropped with the
programs and read back afterwards. As with D3DX's `Begin` under flags 7, a pass saves and restores
no state.

### Instancing

A Ra program whose vertex shader declares the input `TEXCOORD8` (read from its bytecode) is
instanced (contract 3): every entry it draws is an instance, groups of one included, so the depth
prepass and the lit pass place a vertex alike.

- The record (64 bytes): the `World` holder's columns and the `LightMapOffset` holder's value, read
  from the entry's parameter blocks in place; the program's shaders do not read either parameter.
  The records of a frame go to one dynamic vertex buffer, appended without overwrite and discarded
  when full (released before a device reset).
- The stream: the one after the last the material's declaration uses (static meshes keep each
  texture coordinate set in a stream of its own, up to stream 5). The declaration is the material's
  with the four record elements added, made once per declaration; every geometry stream draws
  `INDEXEDDATA` times the group's size, the record stream `INSTANCEDATA`.
- A group: entries with the same program and material (in the depth passes the same material run
  of one draw per mesh, then drawn as one range) whose instance parameters and LOD dither hash
  alike (64 bits; clip-planed entries stay alone). The hash reads the object and lighting blocks'
  plain holders (floats, vectors, matrices, texture pointers) in place, counts the material block
  by its address (a material's entries share it), leaves out the global block (one value per
  pass), and captures the rest through the sink; each program splits its parameters this way once.
  Groups are formed through a hash table, not a sort (`groupFirstSeen`), in the order first met: the
  lit per-material walk on prepass state and hash; the depth walks (prepass, named depth passes,
  each cascade's casters: `drawDepthPassSubmit`) on effect, material, run length and hash, a
  program's groups together. A group's members keep the pass's order. An alpha-tested run of
  several materials draws material by material, each a group. Sorted queues (`drawWalkDefault`)
  keep their order: groups of one.
- Effect-wide for an instanced program: the sun's world direction and the static sun colour
  (`RaSunDirection`, `RaSunColor`). `ViewProjection` is an instance parameter that static meshes
  fill with their view's, so it joins the group key.

### Contribution culling

`renderer.contributionCull` (pixels, default 3, 0 off; owner, 2026-11-19) in place of the distance cull milestone 8
removed: a static mesh whose bounding sphere (its geom's LOD radius) projects to fewer pixels across
than the threshold, at the view's own projection and viewport height, returns from its draw before
any work (`StaticMesh.cpp`, both draw paths). Zoom narrows the projection, so a scope brings small
objects back. Over the 20 % above the threshold the mesh dissolves through `RaLodDither`
(1, -fade), drawing its selected LOD alone; where that band meets a LOD crossfade band the LOD
switch pops. Shadow casters drop with the mesh, without a fade. Left alone: roads, water,
overgrowth (its own fade), and bundled and skinned meshes, so vehicles and soldiers stay visible
at any size. `BF2_DRAW_STATS` counts `contribution culled` and `contribution fading`.

## 15. Shader pipeline

Accepted by the owner, 2026-11-22. Taken ahead of milestone 12 (owner, 2026-11-22): every later milestone edits shaders, and a load
after a shader edit took about 10 minutes on test_bootcamp. Goal: as little shader compiling as
possible, at load and after an edit.

Before: with every effect stale, 426 s of the test_bootcamp load was the main thread waiting for 611
Ra effects compiling on 8 workers (6.3 s each on average, 3,872 s in all), and about 100 s more was
`compileEffectFile` (`particles`, `roadcompiled`, `trail`, undergrowth) on the main thread. The time
was D3DX's April 2005 effect compiler (`d3dx9_24`, `d3dx9_25`): one bundled-mesh permutation took
17.4 s there and 0.56 s with `d3dcompiler_47`.

Built:
1. `d3dcompiler_47` (`D3DCompile`, fx_2_0) compiles every effect (`FxCompiler.cpp`): Ra effects on
   the main thread and on the prefetch workers (a worker per core but one, all in one module), and
   `compileEffectFile`. D3DX stays the effect runtime: `D3DXCreateEffect` loads the output (all 586
   Ra permutations, bm 448, sm 24, stm 114: the same parameters, all 1,489 techniques valid), and
   D3DX's compiler only reads the `.mfx` metadata. The compiler is part of both cache keys.
   `d3dcompiler_47` refuses a parameter named like a technique, which D3DX allowed (`skydome.fx`,
   `postproduction_nv3x.fx`: the parameters were renamed; the engine finds them by semantic).
2. `rashaderbm.fx`'s depth techniques read the lit pass's alpha without its lighting
   (`PS_BundledMesh_Alpha`): the 448 bm permutations compile in 134 s instead of 196 s of CPU. The
   same split in `rashaderstm.fx` saved 5 % and was not kept.
3. `d3dx9_43` (June 2010, the last D3DX; PR's own RendDX9 links it too) is the only D3DX, in place
   of retail's `d3dx9_25` and `d3dx9_24`: RendDX9 and TextureAtlasBuilder link it
   (`RendDX9Abi/D3dx9_43.def`, delay-loaded), the `.mfx` metadata reads through its
   `D3DXCreateEffectCompiler`, and the cache key preprocesses with `d3dcompiler_47`'s
   `D3DPreprocess`. The interfaces the renderer declares are leading parts of the June 2010 ones
   (later SDKs only appended methods). `d3dx9_43` loads all 626 effects `d3dcompiler_47` compiles
   from the shaders, 1,507 of 1,507 techniques valid, as `d3dx9_25` does. Against the `d3dx9_25`
   build: the same Muttrah views (differences only in wind-blown palms and the fountain), Fallujah
   West and Saaremaa load cleanly, frame time the same (7.73 to 7.82 ms against 7.71 to 7.81 ms on
   the `new` path). The D3DX compile path, its per-worker module copies and `BF2_FX_COMPILER` are
   deleted. `BF2_D3DX=sdk` still links the April 2005 SDK (`d3dx9_25`) for retail-parity builds;
   the scripts' builds (G2w seam, editor) keep `d3dx9_25`.

The client needs Microsoft's `d3dx9_43.dll`, its `d3dcompiler_43.dll` (which D3DX's own compiler
loads) and `d3dcompiler_47.dll` beside BF2.exe or in the system (Wine's builtins are vkd3d, not
fxc). Without `d3dcompiler_47` no effect compiles; cached ones still load.
Measured (test_bootcamp `gpm_cq 16`, launch to in game): every effect stale 30 s (about 10 minutes
before), after an edit to `rashaderbm.fx` 22 s, nothing stale 17 s. The compiles run during the
level's console scripts (`part:Objects`); the main thread waits for them for under a second.

Decided from these numbers (2026-11-22):
- No offline shader pack. Building every permutation before launch would save at most the 5 s an
  edit to the bm shader costs, and would need Ra's permutation generation and cache key outside
  the game.
- No permutation collapse for now. Merging the bm flags that only add a texture fetch needs
  per-draw material constants in the draw path for the same 5 s. Worth it if a shader family's
  compile grows again.
- Plain vertex and pixel shaders in place of D3DX effects: deferred (owner, 2026-11-22). Compile
  time is not a reason (the same compiler, the same shaders), and draws already bypass D3DX
  (`Submit.cpp`); it becomes the first task of a move off D3D9, where effects do not exist.

## Effect cache

Ra effects are cached as `.cfx` files per mod (`Documents/Battlefield 2/mods/<mod>/cache`;
retail shared one cache across mods). An entry is keyed (`.cfx.key`) by an FNV hash of the source,
every file reachable through its `#include` lines, the defines, the flags and the compiler, so
editing an included header invalidates every effect that includes it. `effects.lst` lists the
effects a level asked for; at a level's first effect compile the stale ones compile on worker
threads ([perf-load.md](../lanes/perf-load.md#state)). Effects compiled by `compileEffectFile` are cached the
same way under `cache/fxsrc/`. Both compile with `d3dcompiler_47` ([milestone 15](#15-shader-pipeline)).

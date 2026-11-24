# Platform

The `platform` lane: the renderer rebuilt from the ground up on D3D9, as the `render` module, with
every shader written from scratch. Started 2026-11-24 (owner); the `render` lane's open milestones
(12, 17, 21, 22, 23) are paused while it runs. Order and state: [PLAN.md](../PLAN.md#platform); the
open milestone's tasks: [milestones/](../milestones/).

## Goal

A renderer that is simple to read and change, on D3D9 and shader model 3 (a choice, not a stopgap):

- Plain `.hlsl` vertex and pixel shaders; no D3DX effects, techniques or `.mfx` permutation generators.
- A few real variants per shader instead of hundreds of permutations.
- Every render state set by the engine, never inherited from a previous draw.
- One constant and sampler layout shared by C++ and HLSL.
- Features past ps_3_0's limits (32 temporaries, 224 constants, 16 samplers) split across passes,
  not squeezed into one shader.

## Rules

- **From scratch.** Nothing is copied or adapted from the old renderer's code or from PR's shaders.
  The specs are the accepted milestones' documents under [render/](../render/README.md) and the
  published techniques they cite. The old renderer is not worked around either: no bridges, no
  shared state with it, no flag to switch between the two.
- **Its own branch.** All work is on `platform`; `main` keeps the old renderer until the switch
  (milestone 8). On the branch the new frame draws from day one and whatever is not ported yet is
  simply absent. Old renderer code is deleted on the branch as soon as nothing calls it.
- **No "Ra".** That is retail BF2's code name. New code and shaders never use it. It survives only
  where PR's data names it (a mesh's shader name, `RaShaderBM` and the like), read once in the table
  that maps an asset's material to the material model.
- **Names.** C++ in `namespace render`. Shaders named by what they draw (`mesh.hlsl`,
  `terrain.hlsl`, `water_surface.hlsl`, `post_bloom.hlsl`), shared code in `lib/*.hlsli`, functions
  without a prefix (`ComputeLighting`, `SunVisibility`).
- **Public.** The new shaders live in this repository (`Shaders/`): written from scratch against
  BF2's asset formats, they are not PR-derived (owner, 2026-11-24).
- **No D3D9 types** in the module's own structures (`Material`, pipeline states, constant blocks);
  only the code that talks to the device sees them. For tidiness, not a goal.
- **Budgets.** Every pass has a register budget. Over budget, the pass is split, not compressed.

## Design

**Boundary.** The engine keeps asset loading, LOD, culling and the game. Each frame it hands
`render` a list of draw items (mesh, material, transform) and the lights; `render` owns everything
after that: sorting, passes, shadows, lighting, post. Menus, fonts, nametags, the loading screen and
the HUD draw through `render`'s 2D batcher (Geom and GeomMisc stop calling D3D9). D3DX stays only as
a texture and mesh loader.

**Shaders.** `Shaders/` holds plain `.hlsl` files. The engine compiles every shader with
`d3dcompiler_47` (vs_3_0, ps_3_0) in parallel at startup and again on reload: `D3DCompile` on
worker threads, `Create*Shader` on the main thread. No cache unless startup compile passes about
2 s; then one keyed by a hash of the whole shader folder, the defines, the profile and the
compiler. Variants are `#define`s from a short list in C++; only a flag that changes the vertex
layout is a variant, the rest are bool constants or default 1×1 textures.

**Layout.** `shader_layout.h`, included by C++ and HLSL alike and served to the compiler by the
engine (the shaders cannot drift from it; no contract version). Constant blocks by update rate
(`FrameConstants`, `ViewConstants`, `PassConstants`, `MaterialConstants`, `DrawConstants`), separate
vertex (256 registers) and pixel (224) layouts, each block uploaded in one call when it changed. A
fixed sampler table owns every slot and its sampler state (filter, addressing, sRGB); shaders only
declare `sampler2D` at a register.

**Pipeline states.** A handful of named states (opaque, alpha test, glass, additive, depth, caster,
full-screen, 2D), each declaring every state the renderer touches. A cached copy of the device's
state sends only the differences.

**Materials.** One material model for what PR's assets contain (base colour, normal, gloss, GI, env,
alpha), built once per mesh material at load from the asset's shader name and flags. One mesh
shader with variants by vertex layout only: rigid, bundled (part transforms), skinned (bone
weights), instanced.

**Frame.** One C++ function listing the passes in order (no render graph). A fixed set of named
render targets, created at device reset. GPU pass timers in the frame function.

**Splits.** Features past the limits split into passes. Water: a per-frame world-space pass over
the water's texel grid writes slopes, LEAN moments and foam to textures, which the water shader
samples (a screen-space split would lose the 4x MSAA scene's edges: D3D9 cannot read samples).
Load-time bakes (the rivers' flow solve) stay on the CPU; only per-frame work moves to the GPU.

**Tools.** The `tools` lane's console commands (`view.*`, `path.*`, `bench.run`, `tools.*` dumps)
keep working on the branch; each milestone re-attaches what its passes need. A script runs every
saved view and puts it beside `main`'s shot for the owner (the new renderer differs by design, so
a diff is for judging, not pass/fail).

## Milestones

| Milestone | Scope | Done when |
|---|---|---|
| 1. Framework and 2D | The branch; loader, `shader_layout.h`, sampler table, pipeline states, render targets, frame function, register report, GPU timers; the draw-list boundary; menus (Swiff's drawing), fonts, HUD, console, ImGui, loading screen | Main menu to in game, an empty world, the HUD drawn |
| 2. World | View and depth, a plain sky, terrain, the material model and the mesh shader for every mesh kind, sun lighting without shadows | Muttrah City's geometry and materials all drawn |
| 3. Light | Sun cascades and the static cascade, point lights, sky light, fog and atmosphere | The accepted lighting milestones' look, seen by the owner |
| 4. Vegetation and surfaces | Trees, undergrowth, far overgrowth, roads, decals, far terrain | The same for those milestones |
| 5. Water | The world-space surface pass, the shore, rivers | Milestone 23's accepted parts, plus what it gave up for registers |
| 6. Effects | Particles, trails, first person, nametags | The same |
| 7. Sky and post | Clouds (the sun and sky taps back), HDR, bloom, auto-exposure, the grade | The same |
| 8. Switch | The last of the old renderer deleted (D3DX effects, `.mfx`, the render queues and walks, the old shaders' loading); `platform` merged into `main`; the `render` lane resumes on the new base | `main` runs only the new renderer |

Swiff itself is not replaced here: milestone 1 moves only its drawing. Replacing it (RmlUi; PR's
server browser, Esc menu and end-of-round screen) is a later `ui` milestone.

## Open questions

- Whether HUD text depends on the Swiff font wrapper (`GeomMisc/FontSystem/SwiffFont.cpp`); found
  in milestone 1.
- Which asset shader names and flags PR ships: the old cache's 586 permutations list them; the
  material table must cover every one (milestone 2).

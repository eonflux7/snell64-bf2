# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, a shared atmosphere, image-based
ambient specular and run-time sun light. It is a new design: retail behaviour is not a
constraint, and an accepted milestone leaves no legacy path behind it.

The engine side is mostly in `Code/BF2/RendDX9/` (`Frame.cpp`, `BF2Render.cpp`,
`SunShadows.cpp`, `StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shaders are
PR's, kept in a private repository; these documents describe only the contract between the two.

`docs/render/` is the renderer as built, one document per subsystem; each accepted milestone's
facts live in the subsystem it changed. Milestones still open (in progress or paused) each have
their own document in [milestones/](../milestones/), with their tasks and checks. The order and
state of all of them are in [PLAN.md](../PLAN.md#render), what is active now in
[STATUS.md](../STATUS.md).

| Document | Subsystem | Milestones |
|---|---|---|
| This one | The frame in draw order, GPU pass timers, reversed-Z, anti-aliasing | 1, 2, 4, 8 |
| [contract.md](contract.md) | The shader contract (numbered contracts), the shader audit | 6 |
| [submission.md](submission.md) | Draw submission, instancing, contribution culling, the shader pipeline, the effect cache | 9, 15 |
| [shadows.md](shadows.md) | Sun cascades, the static sun and the run-time sun term | 5, 14 |
| [terrain.md](terrain.md) | Terrain in one pass, hex-tiled detail textures | 10, 19 |
| [atmosphere.md](atmosphere.md) | Atmosphere, sky IBL, water reflections, weather per level | 11, 18 |
| [water.md](water.md) | Water | 16 |
| [post.md](post.md) | HDR bloom and auto-exposure | 13 |
| [particles.md](particles.md) | Particle lighting | 7 |

Milestones 1–4 (reversed-Z, linear lighting and the HDR scene, the shared lighting model, the depth
prepass) are described by the sections from [Frame](#frame) on; milestone 4's screen-space sun mask
was deleted in milestone 8.

When a milestone is accepted, its document in `milestones/` is folded into the subsystem's
document here, rewritten as built (a new subsystem gets a new document and a row above), and
deleted.

## Reference views

The owner accepts each milestone in game, by eye, at saved views (`view.save` by the owner;
milestone 11's shots `m11acc-<view>`, earlier `m10-<view>` at 1600x900): Muttrah City and Fallujah
West `sun`, `antisun`, `ridges`, `valley`, `street`; Saaremaa the same with `water` for `street`;
test_bootcamp (`gpm_cq 16`) `gloss`, `gloss2`, `glass`. A view facing the sun has yaw = the survey's
sun azimuth + 180°.

## Frame

The frame module (`Frame.cpp`, `frameRender`) owns the scene, items 1–10 below, and the targets it
draws through. `Renderer::renderFrame` (`BF2Render.cpp`) runs the per-frame updates before it,
passes it a `FrameInputs` (time, the `renderer.draw*` switches, the object drawer and weather
managers), then draws the overlays and presents.

In draw order, for the main world view:

1. **Scene target.** From frame prepare to resolve, the scene draws into an `A16B16G16R16F`
   surface (4x MSAA where the back buffer has it) that stands in for the back buffer
   (`rendererSceneColorSurface`). The HUD draws to the real back buffer.
2. **Sun cascades** (`rendererSunShadowsRender`, milestone 5). Four cascades in a 2 x 2 `R32F` atlas;
   see [Sun shadows](shadows.md#sun-shadows).
3. **Depth prepass** (`rendererDepthPrepass`), just before the world queues flush, after
   overgrowth has queued. Every opaque world-queue entry whose effect has the technique
   `RaDepthPrepass`, and whose prepass draw writes depth, draws into the scene depth near to far
   by distance band (powers of two in metres; within a band by effect, mesh instance and material)
   (the MSAA scene depth when the scene is multisampled), with its linear view depth (`R32F`, 0
   where nothing was drawn) into a target of the same sample count. Terrain draws last, with depth
   write, so the depth test drops what buildings and props in front of it already cover. The linear depth is resolved into a
   single-sample texture bound to s15 for the rest of the frame; the resolve averages an edge
   pixel's samples, so soft particles fade against a depth no surface has.
   In the world pass every prepassed entry draws with depth EQUAL and no depth write, whatever its
   effect states (`stateManagerSetPrepassedDraw`), so each sample is shaded once; terrain's one
   pass (`RaTerrain`) tests EQUAL against its prepass. Not prepassed, and testing and writing depth
   as before: the depth-only queue, undergrowth, roads, the sky, and entries whose effect has no
   `RaDepthPrepass`. First-person models have a prepass of their own (item 6). The effects keep this working by drawing both techniques
   with the same vertex shader function.
   Before it, the cascade pass fills `RASM` and the camera rows are added; after it, `RASM` is
   written to the pixel registers c200–c221.
4. **World passes.** Every lit shader samples the cascades itself at its own world position
   (`RaSunShadow.fxh`), with its triangle's normal (from the position's screen derivatives) for
   the normal offset, turned toward the camera since a derivative normal has no sign of its own.
   Terrain and decals use their own normal, never flipped: at a crest it faces away from the camera,
   and flipped it darkened every silhouette crest. Terrain-lit receivers (terrain, roads) also take
   N.L from it. The lookup position is a `_centroid` interpolant (`SunPos`), so an MSAA edge sample
   stays on its triangle; derivatives come from a non-centroid copy.
5. **Transparents in the 4x scene**, in retail's order: the sky (milestone 11: the model's sky, one
   full-screen pass at the far plane, filling what nothing covers), the near clouds over the scene
   and the sky (milestone 12, [Clouds](../milestones/render-12-clouds.md)), the fog pass, decals, the
   particles drawn before water, and under water the transparency and additive queues. Water draws
   its linear depth first (`rendererLinearDepthWater`: technique `RaDepthPrepass`, testing the
   opaque depth without writing), then its lit pass.
6. **First-person passes**, moved here from after the world transparents. `RASM` is published as
   zeros (`rendererDepthPrepassEndWorldPass`; the frame's values are kept) and soft particles stop,
   so they keep their baked sunlight. After their depth clear, the first-person opaque queues
   prepass (`rendererFirstPersonPrepass`): their own depth, and their linear depth over the
   world's, under alpha-to-coverage; their lit passes test EQUAL. Then the first-person particles
   and transparency queues.
7. **Resolve and depth rebuild** (`rendererSceneResolveForTransparents`). The linear depth (world,
   water, first person) resolves to s15 and the float scene to the single-sample `+0x788`, which
   is the scene target until the end of the scene (`rendererSceneColorSurface`). A full-screen pass
   (PostProduction technique `RaDepthRebuild`) writes depth from s15 into a single-sample depth
   buffer of the scene depth's format, using the world view's projection: first-person depth lies
   nearer than the near plane and saturates to it. `RASM` and soft particles come back on.
   Without the multisampled float scene the rebuild overwrites the bound scene depth in place.
8. **World transparents, single sample**, against the rebuilt depth: the transparency and additive
   queues (above water), particles, the fog transparent pass, weather, rain, the sky's second pass
   and glow. An edge pixel's depth is the average of its samples. `rendererSceneTransparentsEnd`
   zeroes `RASM` again and binds the scene depth back. A world transparent nearer than the weapon
   now draws over it, and smoke draws over first-person transparent surfaces (scope glass).
9. **Particles** sample the cascades per vertex, where the pixel registers and s13 do not reach:
   `RASM` and the atlas are set on them as effect parameters (`rendererBindParticleLighting`).
10. **Post-production** runs every frame: the colour grade is the frame's only encode from linear
   to display, so it cannot be skipped. Its resolve is skipped when item 7 resolved the scene.
   Tinnitus blends over the float scene target first. Just before the grade, the bloom chain, the
   luminance histogram and the auto-exposure run on the scene ([milestone 13](post.md#13-hdr-bloom--auto-exposure)).

Depth is reversed throughout (near 1, far 0) except in shadow-map depth buffers.

When deleting a retail pass, check what state it leaves bound for the passes after it: the
LightManager shadow pass bound the scene depth and RT0, and deleting it broke the scene until
`dc97ef7b`.

## GPU pass timers

`BF2_GPU_TIMERS` ([dev.md](../lanes/dev.md)) and the performance overlay ([ui.md](../lanes/ui.md)) time the frame
in spans, each from its mark to the next, in frame order: `pre-world` (BeginScene to the sun
cascades), `sun clear`, `sun c0`..`sun c3`, `depth prepass` (`terrain depth` within it, terrain's
part), `terrain begin` (the aux queues and terrain water), `world opaque` (with the sky),
`terrain` (`RaTerrain`), `roads`, `sky`, `clouds` (the near clouds, milestone 12),
`world transparent` (the transparents in the 4x scene, item 5), `first person`, `resolve`,
`depth rebuild`, `world transparent 1x` (item 8), `post pass`, `post finish`, `hud` (to Present).
A span includes the GPU's idle time while the CPU is the bottleneck; that usually lands in `hud`.

## Reversed-Z

- Every RenderView projection (perspective, ortho and the linear projection used by the sky dome)
  maps near to 1 and far to 0.
- The StateManager mirrors `ZFUNC` (LESS <-> GREATER) and negates the depth biases while a
  non-shadow depth buffer is bound, re-sending them when the depth buffer changes, and clears
  depth to `1 - z`. Effects keep their forward-Z states.
- XpLightManager marks shadow-map depth buffers as standard depth.
- It needs float depth. DXVK refuses `D24FS8`; `d3d9.useD32forD24 = True` backs `D24S8` with
  `D32_SFLOAT_S8_UINT`.
- Roads take no depth bias. A constant bias is a fixed step in reversed float depth, which is
  centimetres near the camera and metres at range: with the road static meshes' bias (-1e-4) the
  dirt overlays drew over rooftops about 100 m away on Muttrah City. The road shaders (compiled
  roads and road static meshes) scale clip z by 1 + 0.001 instead, which moves the surface 0.1 %
  of its distance toward the camera along the view ray (2 cm at 20 m, 20 cm at 200 m) and leaves
  its screen position alone. The engine no longer sets the road bias for them; the stored value
  (`renderer.roadDepthBias`, `renderer.roadSlopeScaleDepthBias`, the renderer's road-bias slots)
  is read only by the editable roads.

## 8. Anti-aliasing

Fixed 4x MSAA is the one method (no TAA, upscalers or post-process AA; owner); what MSAA alone misses is handled where it starts. The passes are
in [Frame](#frame), the sampler rules in the [shader contract](contract.md#shader-contract).
- Sampler policy: material samplers 16x anisotropic, data samplers point or linear; the
  caps-dependent filter defines are gone (`28487a35`).
- Sun shadows in every lit shader through one include; the screen-space sun mask, its target, s14
  and the prepass normals (s12) are deleted. `RASM` is a fixed register range set once per frame.
  Cascade position, lightmap and atlas UVs are `_centroid`, with derivatives from a non-centroid
  copy (`65760125`).
- The prepass draws into the 4x scene depth and prepassed draws test EQUAL ([Frame](#frame) item 3,
  `f11bdfe3`). First-person models prepass in the 4x scene, water adds its linear depth, and the
  world transparents draw single sample against a rebuilt depth (items 5–8, `b47ac87f`, `0f10eb41`).
- Alpha-to-coverage in the mesh prepass (retail's `ATOC` bracket, which DXVK honours); the lit
  passes drop their alpha test; alpha is sharpened to a one-pixel ramp (`f9086318`). Not done: the
  mip-loss alpha correction (needs texture sizes from the engine).
- LOD policy: terrain draws LOD 0 at every distance with no geomorph (`ee0e7ed8`); bundled meshes
  always LOD 0, static switch distances x3, mesh shading LODs deleted, textures full resolution at
  quality 3 (`98165f0b`); objects cull only on the frustum, far plane = view distance
  (`b3e44a8d`). The remaining static mesh and overgrowth switches crossfade with a 4x4 Bayer
  dither over 10 % of the switch distance (`RaLodDither`, c222); far overgrowth cells dissolve over
  the last tenth of the view distance; near and far overgrowth share one lighting model
  (`a7d9e0e1`, `1e7a1c89`, `680d8890`).
- Dropped: thin-geometry widening (cables break into dashes beyond about 100 m, as in retail).
  Moved to milestone 13, then dropped there: the compressed (Karis) resolve.

Limits: shading once per pixel leaves glossy surfaces sparkling (specular AA is milestone 11). The
default video profile still picks 8x antialiasing while the scene runs 4x. `Ra_LodDitherClip` runs
in every lit shader even at (0, 1), costing early depth rejection where there was no clip before.

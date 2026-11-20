# Renderer pipeline

What the `render` lane's renderer does today: the accepted milestones, the frame in draw order, and
the contract between the engine and the (private) shaders. The plan and the milestones still to do
are in [renderer.md](renderer.md), with each milestone's commits. Update this document when a
milestone is accepted.

## Accepted milestones

Milestones 1–4 (reversed-Z, linear lighting and the HDR scene, the shared lighting model, the depth
prepass) are described by the sections from [Frame](#frame) on; milestone 4's screen-space sun mask
was deleted in milestone 8.

### 5. Sun cascaded shadow maps

See [Sun shadows](#sun-shadows). Accepted at the values its console variables now default to.

### 6. Shader audit and cleanup

An inventory of the shader set (every effect and header, its techniques and permutations, which of
them a PR session loads, what later milestones replace) lives with the private shaders. Liveness
came from `BF2_DRAW_STATS` ([flags.md](flags.md)) on maps that between them cover water, foliage,
buildings and vehicles. What it removed from the engine:
- The toggles fixed when milestones 1–4 were accepted (reversed-Z, log depth, gamma-space lighting)
  and their dead branches.
- BF2's directional (sun) shadow maps: the directional pass in `XpLightManager.cpp`, its records and
  every receiver. That is the mesh shadow permutations (bundled and skinned mesh `hasshadow` /
  `hasshadowocclusion` / `hashemiocclusion`, static mesh `shadow`, the `*Shadowed` tree
  techniques), terrain water's record matrices, undergrowth's `_ds` techniques, overgrowth's record
  attach, and the decal renderer's shadowed batch kinds (decals draw pass 0 only).
- BF2's point and spot shadow maps. Retail kept shadow records only for lights with a non-zero
  shadow policy, which is zero outside the expansion pack, so a PR session never drew this path.
  Deleted: the records, the shadow atlas and its device-lost listener, the casters'
  `DrawShadowMap` draws on bundled, skinned and static meshes (with the shadow effects they
  loaded), the static mesh lightmap silhouette, and the record receivers in static meshes, terrain
  (the shadow-range gather and batch draws, the spot-light pass) and overgrowth. The XP light
  coordinator stays: it still selects the point and spot lights that light geometry.
- Kept as empty bodies for the vtable layout: the LightManager's `renderShadowMaps` and
  `registerShadowEffect`, the meshes' `drawShadowMap` / `drawSilhouette`, overgrowth's
  `collectShadowGeometry`. The shadow-map console setters only store their value. The deleted pass
  also bound the scene depth-stencil and colour target 0 on exit each frame; the renderer now binds
  them itself where it was called (`dc97ef7b`), and the sun cascades clear their standard-depth mark
  when they restore the scene depth (`e7623d07`).
- One terrain path. `Low_Terrain` had no `RaDepthPrepass` or `RaShadowDepth` technique, so the
  engine no longer enters it: not at load, not from the terrain-quality video option, not as a
  fallback. Its `m_lowQuality` branches went in milestone 10 ([Terrain](#terrain)).

### 7. Particle lighting

The four particle effects (`particles.fx`, `nonscreenalignedparticles.fx`, `trail.fx`,
`meshparticlemesh.fx`) light in linear on the level's one sun, through one shared function in the
shaders. Every particle renderer calls `rendererBindParticleLighting` as it binds its effect (sun,
sky, sun direction, `RASM`, the cascade atlas, soft-particle constants; see the
[shader contract](#shader-contract)).
- Only the authored tint is decoded; light multiplies it in linear. Lit tints are capped at 1 (an
  albedo): PR's tints above 1 compensated for its dim gamma-space sun and glowed once decoded.
- Lighting is per vertex. A billboard lights at its centre, with one cascade lookup however much it
  overdraws; beyond the cascades, and outside the main world pass, the baked term (hemi map alpha
  plus the template's altitude offset) is the sun visibility. Billboards and trails have no normal:
  full sun plus the hemisphere's average, whose ground bounce takes the hemi map's colour as static
  meshes do. Mesh particles use their normal.
- Emissive: additive effects, and the unlit part of a template's light factor, show their tint
  times 2.38 (2^1.25, undoing the frame's exposure, so they reach the tonemap at their authored value).
- A template that sets no light factor is lit fully. Retail gave it the level's
  `defaultEffectLightAffectionFactor`, which eight PR levels set to 0 (Kashan, Adak, Burning Sands,
  Icebreaker, Iron Ridge, Khamisiyah, Masirah, Road to Damascus; Fool's Road 0.7), so their effects
  drew unlit and flat. Templates that set a factor keep it.
- Soft particles: lit and additive particles fade over `renderer.softParticleDistance` (default
  1.5 m, range 0.1–20) in front of the prepass depth (s15). Alpha-blended passes fade alpha, `ONE,ONE`
  passes fade colour; `DiffuseWithZWrite` stays hard. No fade on the sky (depth 0) or outside the
  main world pass (first person, other views).
- Screen-aligned sprites turn against the camera's roll, so smoke stays upright when an aircraft rolls.
- Thermals keep PR's particle output exactly: the old gamma-space formula with `EffectSunColor` /
  `EffectShadowColor`, the level's light factor, hard edges. The thermals rewrite's smoke (paused) replaces it.

Limits: a billboard or trail lights at one point, so a streak that starts in shade stays shaded where
it rises into the sun. Water and undergrowth draw no prepass depth, so particles don't soften against
them; a fringe remains at depth edges, where the fade follows the resolve's averaged depth
(milestone 8). Fire reads
red-orange rather than yellow-orange: the PBR Neutral tonemap keeps hue as it compresses highlights,
where PR's per-channel clip pushed bright orange toward yellow (AgX was tried and not preferred).
That is the grade's, not this milestone's.

### 8. Anti-aliasing

Fixed 4x MSAA is the one method; what MSAA alone misses is handled where it starts. The passes are
in [Frame](#frame), the sampler rules in the [shader contract](#shader-contract).
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
  Moved to milestone 13: the compressed (Karis) resolve.

Limits: shading once per pixel leaves glossy surfaces sparkling (specular AA is milestone 11). The
default video profile still picks 8x antialiasing while the scene runs 4x. `Ra_LodDitherClip` runs
in every lit shader even at (0, 1), costing early depth rejection where there was no clip before.
GPU timings: [perf-frame.md](perf-frame.md#after-render-milestone-8).

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
   see [Sun shadows](#sun-shadows).
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
   full-screen pass at the far plane, filling what nothing covers), the fog pass, decals, the
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
   Tinnitus blends over the float scene target first.

Depth is reversed throughout (near 1, far 0) except in shadow-map depth buffers.

## GPU pass timers

`BF2_GPU_TIMERS` ([flags.md](flags.md)) and the performance overlay ([ui.md](ui.md)) time the frame
in spans, each from its mark to the next, in frame order: `pre-world` (BeginScene to the sun
cascades), `sun clear`, `sun c0`..`sun c3`, `depth prepass` (`terrain depth` within it, terrain's
part), `terrain begin` (the aux queues and terrain water), `world opaque` (with the sky),
`terrain` (`RaTerrain`), `roads`,
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

## Sun shadows

`SunShadows.cpp`. Casters come from a **submission context**: each mesh type's normal submission
takes its cull volume and queue target from a pass context, so the shadow pass re-runs submission
against the cascade volume into a caster list (`rendererShadowCasters`). Camera queues are not
reused, because meshes cull per material against the camera. Casters draw with each Ra effect's
`RaShadowDepth` technique; terrain draws its own (`Terrain::drawShadowCasters`).

Objects come from the object manager's roots-only grid query against the light volume (the
light-space box around the four cascade spheres); hierarchies are expanded by the drawer. Seen in
game on Muttrah (`2860a74a`): a supply crate's shadow no longer depends on view direction. Objects
have no distance cull (milestone 8), so a caster never depends on the camera's distance to it.

Each cascade draws only the casters that can shadow a receiver in its own slice
(`ShadowCascadeCull`): in light space, the 2D convex hull of the slice's eight corners, pushed out by
the receivers' filter (2.5 cascade texels) plus their normal offset, and in depth from the projection's
sun-side bound to the slice's farthest corner. Casters carry a world bounding sphere, the geometry's
local box sphere through its world transform; one without bounds draws in every cascade.

The gather applies the same test once for the whole shadow range (the hull of the slice from the
near plane to the shadow distance, the widest cascade's margin, the nearest sun-side bound), which
holds every cascade's own cull, before any work on an object: an object stays when its visual
bounding sphere (the grid query's) or one of its geometries' spheres touches the range. Only those
are brought up to date (visual update, geometry update) where the camera pass did not see them, and
submitted. The light volume holds 1.5 to 2.3 times as many objects as reach a cascade.

- Four cascades over the view depth up to 400 m, split by a blend of
  logarithmic and uniform (lambda 0.75). Each cascade but the last also covers the first 20 % of
  the next slice; there the receiver blends from the finer cascade to the coarser one, so splits
  don't show.
- Each cascade is a bounding sphere of its view slice, so its size does not change as the camera
  turns, and its centre is snapped to whole texels across the light, so static shadows don't crawl.
- Casters up to 500 m toward the sun beyond the sphere still draw.
- The atlas stores the light-space depth of the nearest caster (0 = sun-side bound, 1 = far side).
  Receivers compare and filter (3 x 3 tent PCF) and, past 90 % of the last split, blend to the
  baked lightmap sun.

Tuned live through console variables, which clamp to their ranges and default to the values the
milestone was accepted with:

| Variable | Default | Range | What |
|---|---|---|---|
| `renderer.sunShadows` | 1 | 0–1 | Cascades drawn; off, surfaces take the baked lightmap sun |
| `renderer.sunShadowDistance` | 400 | 50–2000 m | View depth the cascades reach |
| `renderer.sunShadowResolution` | 2048 | 512–4096 | Texels per cascade side, a power of two; a change re-creates the atlas |
| `renderer.sunShadowCasterReach` | 500 | 0–2000 m | Caster reach toward the sun past a cascade's sphere |
| `renderer.sunShadowSplitLambda` | 0.75 | 0–1 | Splits: 0 uniform, 1 logarithmic |
| `renderer.sunShadowCascadeBlend` | 0.2 | 0–0.5 | Part of the next slice each cascade also covers and blends over |
| `renderer.sunShadowNormalOffset` | 1.5 | 0–4 texels | Receiver offset along its normal |
| `renderer.sunShadowDepthBias` | 1.0 | 0–4 texels | Receiver offset toward the sun in depth |

Their help lines, for the debug menu, come from `rendererVariableHelp` (`RendererConsole.cpp`).

### Cascade constants (`RASM`, 22 float4, pixel registers c200–c221)

| Rows | Contents |
|---|---|
| 0–3 | Camera: position; forward; right / P00; up / P11 |
| 4 + 3i .. 6 + 3i | Cascade i: world to atlas texel (x, y) and light depth |
| 16 | View depth each cascade's slice ends at |
| 17 | `.xyz` direction toward the sun; `.w` view depth where the blend to baked sun starts |
| 18 | Each cascade's texel size in world units |
| 19 | Each cascade's light depth per metre |
| 20 | `.x` one atlas texel in UV; `.y` normal offset and `.z` depth bias, in cascade texels |
| 21 | View depth each cascade's coverage ends at (its blend into the next slice) |

All zero outside the main world pass, and when no cascade was rendered: nothing is covered.

The engine owns the registers: it writes them once per frame through the StateManager's constant
mirror (`stateManagerSetSunCascadeConstants`), and the StateManager drops the part of any effect's
pixel constant write that falls in the range. Each receiver declares `RASM` at `register(c200)`,
so D3DX would otherwise upload the parameter's own zeros at every pass. No pixel shader may place
anything else there.

## Shader contract

The contract has a version, `RA_CONTRACT_VERSION` in `RendDX9Base.h`. The engine passes it as the
define `RA_CONTRACT` to every effect it compiles: Ra effects get it with their profile defines,
every other effect through the ShaderManager define table. The shaders' core header stops with
`#error` when it is missing or older than they need, so a shader set never runs on an engine
without its contract. Raise the version in the change that alters what the shaders may rely on,
and add its row here.

| Version | Adds |
|---|---|
| 1 | The contract as of render milestone 8: everything below |
| 2 | Material depth states by semantic: `RaAlphaTest`, `RaAlphaRef`, `RaDepthWrite` (render milestone 9, [Submission](#submission)) |
| 3 | Instanced static meshes: a vertex shader input `TEXCOORD8`–`TEXCOORD11` is the instance record (the world transform's three columns, the lightmap offset); positions from the instance parameter `ViewProjection`; the sun in world space by semantic `RaSunDirection`, `RaSunColor` ([Instancing](#instancing)) |
| 4 | Terrain light (render milestone 10): `TEXLAYER1` is the patch lightmap, read at the colormap's UV, instead of the accumulation target; the point lights reaching the patch in `RaTerrainLightPos` (`float4[4]`, position and 1 / range²), `RaTerrainLightCol` (`float4[4]`, authored colour), `RaTerrainLightCount` (`int`) |
| 5 | Terrain in one pass (render milestone 10): technique `RaTerrain` (pass 0, depth EQUAL, no blend) on the terrain effect; per patch `RaTerrainLayerCount` (`int`), `RaTerrainLayerTop`, `RaTerrainLayerSide` (`float4[6]`, atlas cell scale and offset), `RaTerrainLayerTiling` (`float4[6]`, side tiling, top tiling, side y offset), `RaTerrainLayerWeight0`/`1` (`float4[6]`, channel in weight map 0 or 1), `RaTerrainLayerAtlas` (`float4[6]`, top atlas, side atlas, planar), `RaTerrainUnderWater` (`bool`); textures `RaTerrainAtlas0`–`2` (detail atlases, wrapped in the shader), `RaTerrainWeight0`/`1` |
| 6 | Atmosphere (render milestone 11, task 2): effect `Shaders/RaAtmosphere`, technique `RaAtmosPerspective` (the aerial-perspective atlas, one full-screen pass drawn by the engine each frame); parameter `RaAtmosphere` (`float4[16]`, layout in the effect and `Atmosphere.cpp`'s `fillConstants`). The transmittance, multiple-scattering and sky-view tables are built by the engine; how they are sampled joins the contract with the task that first does (3) |
| 7 | The model's sky (render milestone 11, task 3): technique `RaSky` on the sky dome effect, one full-screen pass at the far plane (depth tested, not written) drawn in the dome's place; while it draws, the painted dome, its cloud layers and the flare sprite (`SkyDomeSunFlare`) are not drawn, and the flare's occlusion test runs as before. Constants: `RaSky` (`float4[3]`: [0] x 1 under water (the pass draws the water's fog colour), z, w cos of the sun disc's radius (0.27°) and of its soft edge; [1].rgb the disc's radiance, the authored sun over its solid angle, luminance capped at 100; [2] the viewport x, y, 1 / width, 1 / height), `RaAtmosphere` (as in contract 6, camera rows included), `RaSkyViewTable` (`texture`, 192x108 `A16B16G16R16F`, linear clamp: x the azimuth from the sun as sqrt((1 − cos)/2), y the zenith angle squared toward the horizon from both sides) |
| 8 | Aerial perspective (render milestone 11, task 4): the atlas (contract 6) is bound to the reserved sampler s9 (linear, clamp) after it draws, and pixel register c223 is the engine's `RaAerial` (1 / the viewport's width and height, 1 / fog end, w 1 while the atlas holds the world view's haze; zero in the first-person passes, under water, with `renderer.atmosphere 0`), published with `RASM`. Lit shaders fog through `Ra_GetAerial` (VPOS and the distance pick the atlas texel; w 0 or thermals: retail's fog curve). `RaAtmosPerspective` also gets `RaSkyViewTable`: full haze is the sky's radiance along each ray. `RaAtmosphere` rows 8–10: [8] unused (the steepening, removed at contract 9), [9].rgb the sun at the ground, [10].rgb the sky light |
| 9 | View distance (render milestone 11, task 5): `RaAerial.w` is 2 while the radial cut applies (`renderer.viewDistance`, the world view with the atlas). `Ra_ViewDistanceFade` (1 inside, 0 at fog end and beyond, over the last 15 % by distance from the camera) and `Ra_ViewDistanceClip` (that fade against a 4x4 Bayer threshold, `Ra_Bayer4`) are called by every opaque world effect's main pixel shader, which its depth prepass calls too; blended effects (particles, trails, decals, blended roads) take the fade on alpha. The atlas is no longer steepened: the haze stays exponential up to fog end |

Samplers 9, 13 and 15 are reserved for renderer frame resources, and pixel registers c200–c223 for
`RASM`, `RaLodDither` and `RaAerial`. No effect may use them for anything else.
Every other sampler states `SRGBTexture` in its `sampler_state`: TRUE where it reads colour (the
hardware decodes before filtering), FALSE for data and outside the scene. D3D9 sampler state carries
over between effects, so neither value is left to default. The engine binds s9, s13 and s15 with it FALSE.

Every `sampler_state` also states its filter. A material sampler (a colour, normal or detail texture
mapped onto world geometry) is 16x anisotropic: min `ANISOTROPIC`, mag and mip `LINEAR`. A data
sampler (lightmaps, light and blend maps, cube and lookup maps, render targets, UI) states point or
linear. The engine defines no filter: retail's caps- and setting-dependent `FILTER_*` defines and
`IS_NV4X` are gone, so the texture filter quality setting changes nothing and the effect cache
directory no longer carries it (`_3153_0`).

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s9 | Aerial-perspective atlas (1024x32: 32 distance slices of 32x32 screen tiles) | `A16B16G16R16F` | After the atlas pass (start of the depth prepass), rest of the frame; read only while `RaAerial.w` > 0 |
| s13 | Sun cascade atlas | `R32F` | From the cascade pass, rest of the frame |
| s15 | Linear view depth | `R32F` | After the prepass, rest of the frame; particles read it only while `RaSoftParticles.x` > 0 |

Techniques the engine looks up by name:

| Technique | In | Draws |
|---|---|---|
| `RaDepthPrepass` | Ra mesh effects, terrain | Linear depth to COLOR0 |
| `RaShadowDepth` | Ra mesh effects, terrain | Light depth into the atlas, with `RaShadowViewProj` |
| `RaTerrain` | Terrain | Every detail layer in one pass (contract 5, [Terrain](#terrain)) |

Passes the submission layer draws (`RaDepthPrepass` and `RaShadowDepth` of the Ra mesh effects)
take every render and sampler state as a constant, except three a material decides, which follow
these parameters per draw: `RaAlphaTest` (`bool`, ALPHATESTENABLE), `RaAlphaRef` (`int` 0–255,
ALPHAREF), `RaDepthWrite` (`bool`, ZWRITEENABLE). A pass that fixes one of them (leaves:
`AlphaTestEnable = TRUE`) keeps its own value. They set no fixed-function state, and a texture
reaches them only through a sampler's `Texture = (param)`.

Parameters the engine sets by semantic:

| Semantic | Type | Set on |
|---|---|---|
| `RaShadowViewProj` | `float4x4` | Ra effects drawing `RaShadowDepth`: the cascade being drawn |
| `RASM` | `float4[22]` | The particle effects at every bind (`rendererBindParticleLighting`), which sample per vertex. Pixel shaders read it from c200–c221 instead and nothing sets it on them |
| `SUNCOLOR` (`TerrainSunColor` in the Ra constants) | `float3` | Terrain, road and undergrowth effects: the level's one sun, `Lightmanager.staticSunColor` as authored (undergrowth times its `LightingScale`). Retail passed `terrain.sunColor / 4`, balanced against the statics by hand per level |
| `RaSunColor`, `RaSkyColor` | `float3` | The four particle effects (`rendererBindParticleLighting`, called by each particle renderer as it binds its effect): `Lightmanager.staticSunColor` and `staticSkyColor` as authored, the one sun and sky every lit surface uses |
| `RaSunDirection` | `float3` | The particle effects: unit vector toward the sun (`-Lightmanager.sunDirection`) |
| `RaSunCascadeAtlas` | `texture` | The particle effects: the cascade atlas, which they sample per vertex. A vertex shader cannot read s13, so the atlas reaches them as a texture parameter and D3DX binds the vertex sampler |
| `RaSoftParticles` | `float4` | The particle effects: `.x` 1 / `renderer.softParticleDistance` while the main world pass draws over this frame's prepass depth, else 0 (no fade); `.zw` 1 / the depth target's size |
| `TemplateParameters` `color1.a` (light factor) | `float` | The particle templates, as retail, except a template that sets no light factor: `-1 - Lightmanager.defaultEffectLightAffectionFactor` (`particleTemplateUnsetLightAffection`). The shaders light it fully; thermals read back the level default |
| `EffectSunColor`, `EffectShadowColor` | `float3` | The particle effects, as retail: read only by the thermals path, which keeps PR's particle formula, and (`EffectSunColor`) as the fill scale of the `ShowFill` techniques |
| `Lights[0].color` (sun record), `SunColor` (decals) | `float3` | Bundled meshes (cockpits included), skinned meshes, trees and decals: the same one sun, `Lightmanager.staticSunColor`. Retail gave them `Lightmanager.sunColor`, the cockpit gate's colour, `treeSunColor` or the sky light's colour, each authored against a separate ambient (on Kashan the dynamic sun is 0.6 against a static 0.73 doubled). Unscaled: retail also multiplied bundled and skinned meshes' sun by the baked simple shadow map, which the cascades replace and which blacked out meshes under roofs once the sky came from the sun |

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

## Terrain

Render milestone 10 (accepted by the owner, 2026-11-20). Terrain draws in one pass at full detail
everywhere. Retail drew a lightmap pass (`ZFillLightMap`: depth and the lightmap packed to `[0,1]`
into an accumulation target, `+0x774`), additive point-light passes into that target, then per
patch one additive pass per detail layer on the near nodes and a low-detail pass on the far ones,
each reading the target projected and fogging again where they overlapped. Roads read the same
target.

- **Atlases** (`Geom/TerrainAtlas.cpp`, built on the terrain's first `update` after a load,
  managed pool): one detail atlas per (format, size) of the level's detail and side textures, each
  cell the texture 2x2 shifted half a tile, copied per DXT block, so the shader wraps it
  (`atlasUv = frac(uv) * scale + offset`, `tex2Dgrad` with the unwrapped gradients); one lightmap
  atlas of every patch's lightmap, cell = patch grid position. Console `terrain.atlasDump` writes
  them as `.dds`.
- **Depth**: the visible patches draw `RaDepthPrepass` at the end of the depth prepass
  ([Frame](#frame), item 3).
- **`Terrain::drawMain`** (the +0xB8 arm) flushes the aux queues and gathers the point lights per
  patch (`gatherPointLights`: the culler's sphere gather per light, the nearest four to the patch
  centre; the static frame lights under ShaderVersion 1, else the visible lights of mask 2313).
- **`Terrain::drawFinal`** (the +0xBC arm) draws technique `RaTerrain`, one draw per patch over its
  visible nodes' LOD 0 ranges (`QuadPatch::drawNodes`, adjacent ranges merged), shader contract 5:
  up to six layers in layer order from the atlases, blended by weight; the low-detail map modulates
  them, fading in with distance; fog once on the weight-normalised colour; layer 6 (under water)
  blends the water colour by depth. Light (contract 4): the patch lightmap at the colormap's UV
  (centroid), the sun cascades, the per-patch point lights. Without the atlases or the technique
  terrain draws no colour.
- **Roads** (compiled roads, `roadcompiled.fx`; road static meshes, `rashaderroad.fx`) light from
  the lightmap atlas by world position (`Ra_SampleTerrainLightAtlas`; grid and (GI colour, 1) in
  the Ra records `TerrainLightAtlasGrid`/`TerrainLightAtlasGI`, retail's accumulation projection
  pair, and on compiled roads `TEXPROJSCALE`/`TEXPROJOFFSET`). No distance cull and no fade: they
  draw to the far plane. Dynamic point lights do not reach roads.
- **Gone**: the accumulation target's use and its `AccumulationMap` record, `Hi_Terrain` and its
  passes, the culler's detail distance and `detailNear`, the console properties
  `terrainCuller.lodSwitchDistance` and `lod0SwitchStart` (the road view distance and fade) and
  the video option's culler distances, the low-quality branches and the Renderer's terrain-quality
  slots (+0x98 does nothing, +0x9C answers false), the dynamic shadow-range index buffer (+0x368).

Frame time on the benchmark paths, old path against the one pass (same build and session,
Conquest 64, 1920x1080 4x, 3 runs each, 2026-11-20): Muttrah City `bench1` 7.81 to 7.92–8.02 ms
(draws 3324 to 3465, Mprims 3.18 to 2.91), Fallujah West `bench` 14.92 to 15.17–15.27 ms (4481 to
4489, 2.88 to 2.75), Saaremaa `bench1` 5.18 to 5.20–5.21 ms (1640 to 1630, 2.56 to 2.29; the frame
GPU span 4.74 to 4.38–4.48 ms). Every frame CPU bound; the 0–2 % is within the spread between runs.
After the deletion: 8.39 / 8.48 (another load on the machine during the run), 15.34 and 5.22 ms.

## Effect cache

Ra effects are cached as `.cfx` files per mod (`Documents/Battlefield 2/mods/<mod>/cache`;
retail shared one cache across mods). A cache entry is keyed by an FNV hash of the preprocessed
source and its defines (`.cfx.key`), so editing an included header invalidates every effect that
includes it. The effect preprocessor loads its own D3DX handle.

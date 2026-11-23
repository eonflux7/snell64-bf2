# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, and later on a shared atmosphere,
image-based ambient specular and screen-space ambient occlusion. It is a new design: retail
behaviour is not a constraint, and an accepted milestone leaves no legacy path behind it.

The engine side is mostly in `Code/BF2/RendDX9/` (`Frame.cpp`, `BF2Render.cpp`,
`SunShadows.cpp`, `StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shaders are
PR's, kept in a private repository; these documents describe only the contract between the two.

This document is the renderer as built (the frame in draw order, the shader contract) and every
milestone's design, accepted or still to do. Their order and state are in
[PLAN.md](../PLAN.md#render); the current milestone's tasks in [STATUS.md](../STATUS.md).
When a milestone is accepted, its section is rewritten as built.

## Milestones

Milestones 1–4 (reversed-Z, linear lighting and the HDR scene, the shared lighting model, the depth
prepass) are described by the sections from [Frame](#frame) on; milestone 4's screen-space sun mask
was deleted in milestone 8.

### 5. Sun cascaded shadow maps

See [Sun shadows](#sun-shadows). Accepted at the values its console variables now default to.

### 6. Shader audit and cleanup

An inventory of the shader set (every effect and header, its techniques and permutations, which of
them a PR session loads, what later milestones replace) lives with the private shaders. Liveness
came from `BF2_DRAW_STATS` ([dev.md](dev.md)) on maps that between them cover water, foliage,
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

Fixed 4x MSAA is the one method (no TAA, upscalers or post-process AA; owner); what MSAA alone misses is handled where it starts. The passes are
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
  Moved to milestone 13, then dropped there: the compressed (Karis) resolve.

Limits: shading once per pixel leaves glossy surfaces sparkling (specular AA is milestone 11). The
default video profile still picks 8x antialiasing while the scene runs 4x. `Ra_LodDitherClip` runs
in every lit shader even at (0, 1), costing early depth rejection where there was no clip before.

### 10. Terrain in one pass

Accepted by the owner, 2026-11-20; as built in [Terrain](#terrain).
Survey of the three maps (task 1, 2026-11-20; console `terrain.survey`):

| | Muttrah City | Fallujah West | Saaremaa |
|---|---|---|---|
| Patches (128 m) | 8x8 | 4x4 | 8x8 |
| By layer count 1/2/3/4/5/6/7 | 15/17/1/2/17/5/7 | 0/3/3/2/5/2/1 | 34/0/2/7/14/5/2 |
| With planar / env / layer 6 | 46 / 0 / 39 | 16 / 0 / 4 | 30 / 0 / 56 |
| Detail textures | 6, 512² m7, DXT1/DXT5; detail 0 planar | 6, 512² m7, DXT1/DXT5, all planar | 6, 512² m7, DXT1/DXT3/DXT5; detail 0 planar |
| Weight maps | 256², R5G6B5 or DXT1, 1 mip | same | same |
| Colormap / lightmap / low-detail | 512² DXT1 / 1024² DXT1 / 256² DXT1 | low-detail mode 3, 512² R5G6B5 | 512² DXT1 / 1024² DXT1 (32 patches 8x8 dummies) / 256² DXT1 |
| Point lights on terrain | none (`drawMaskLights` path, shader version 0) | none | none |
| `bench1`/`bench` frame | 10.66 ms | 20.51 ms | 6.34 ms |
| GPU: lightmap / depth / detail | 0.398 / 0.168 / 0.337 ms | 0.490 / 0.200 / 0.623 ms | 0.312 / 0.145 / 0.321 ms |
| GPU: roads | 0.095 ms | 0.102 ms | 0.014 ms |
| Draws a frame: pass 0 / 3 / 4 / 5 / 14 | 12 / 13 / 18 / 3 / 3 | 9 / 6 / – / 24 / – | 18 / 17 / 2 / 1 / 3 (average) |

Patches with one layer and no colormap are sea or off-map (Saaremaa's 34). No environment-mapped
layer and no point light reaches terrain on the three maps. The GPU and frame rows are the old
path, measured with `GPU=1`.

### 11. Atmosphere, sky IBL, water reflections

Accepted by the owner, 2026-11-22: a more realistic outdoor image, judged by eye at the reference
views of Muttrah City, Fallujah West and Saaremaa against milestone 10. Sky, sun, haze and sky
light come from one physical model under the level's authored sun; glossy surfaces and water
reflect it. No switch is left: the retail dome, fog curve (except where noted), flat sky light and
environment maps are gone from the lit path.

**One light, physical** (owner, 2026-11-21). The sun at the ground is the one every lit surface
takes: `Lightmanager.staticSunColor` decoded as the shaders decode it (colour^2.2 x 2^2.2), the
anchor the lightmaps were baked under. Everything else is physical under it, with no per-level gain:
the sky, the sun disc, the haze's light, the sky cube and the sky irradiance. The frame grade's
exposure is 0 stops; auto-exposure is milestone 13's. On test_bootcamp the open sky lights an
up-facing surface at 0.065 of the sun, a clear day in clean air.

**Fit at load** (`Atmosphere.cpp`, about 250 ms). Two layers, since one atmosphere dense enough
for PR's fog end (0.5–2 km) would turn the sky into haze:
- *Sky atmosphere:* Earth's air on every level (Rayleigh, Mie, ozone at Earth's densities), clear
  until milestone 12's clouds. Sunlight above it = the authored sun / the transmittance at the sun's
  elevation, clamped to at least 0.1 per channel (low suns, night layers).
- *Ground haze* (replaced by [milestone 18](#18-atmosphere-rework)'s aerosol): density from a visibility, the same on every level (`renderer.hazeVisibility`,
  8 km: a level ray at the reference height keeps 2 % contrast there, 3.912 / V per metre), albedo
  from the authored fog colour, density falling with height (`renderer.hazeFalloff`, 1200 m).
  Realistic, mostly in the distance (owner, 2026-11-22); it was optical depth 1 at fog end (400 m
  falloff), 2.7 km of visibility on Muttrah City's 700 m. Exponential in optical depth throughout,
  past fog end too; retail's fog start is not used.
- *Tables,* built on the CPU by the code the fit evaluates, in managed textures that survive device
  resets: transmittance 256x64, multiple scattering 32x32, sky view 192x108 (at the terrain's mean
  height). D3D9 has no compute and no volume render targets, and pixel-pass versions of these
  loops came out wrong under DXVK.
- *Sky cube* (`SkyCube.cpp`, 108 ms on Muttrah City): 128 per face, `A16B16G16R16F`. Above the
  horizon the model's sky; below it the colormaps' mean albedo lit by the sun and the sky, hazed
  from 10 m up to the horizon's radiance (sky alone below the horizon made every gloss look
  chrome). Prefiltered GGX, one mip per perceptual roughness (64 samples per texel), and its
  irradiance as SH9 (within 1 % sideways, 2–13 % straight up and down). A split-sum BRDF table
  (64x64).

**Per frame.** The aerial-perspective atlas (32x32 x 32 depth slices in a 2D atlas, a pixel pass
after the sun cascades) on s9; every fogged shader reads it per pixel (`Ra_GetAerial`). The haze's
transmittance comes from the tile; its light from a 33rd block, per tile column and elevation
(contract 20), by each pixel's own ray: in full haze far land takes the sky's colour right behind
it, so the sky's quick change just above the horizon no longer shows as a band across the mountains
a tile off the horizon. View- and clip-space callers (particles, decals) keep the tile's light
(`Ra_GetAerialView`). The clear sky is not hazed (its light is the haze's). The clouds are, each to
its own distance with the density falling with height along the way, in the same air-light
function as Earth's air (contract 25), so near and far clouds fade alike and match the ground at
the same distance. Hazing the whole ray to infinity instead hid every far cloud under a crisp
near bank (tried, 2026-11-22 and 2026-11-23). Retail's fog
curve stays under water, in first person and in thermals. The sky is one full-screen pass at the
far plane in the dome's place (`SkyDome.fx` technique `RaSky`): the sky view plus a limb-darkened sun
disc capped for the MSAA resolve; under water the water's fog colour. The painted dome, its cloud
layers and the flare sprite are not drawn; the flare's occlusion query still runs (nothing reads it
now). Nothing draws the sky before the first fit.

**View distance** (owner, 2026-11-20). Fog end stays each level's gameplay limit; what changed is how
it looks and that it is the same in every direction.
- Objects are cut at the radial distance from the camera, not the flat far plane (retail's corners
  reached 30–40 % further): the last 15 % dithered out on a 4x4 Bayer threshold
  (`Ra_ViewDistanceClip`), blended effects faded by alpha. The world projection is unbounded
  (reversed float depth); objects still cull at the view distance.
- Past it the land goes on: a scenery frustum to 40 km for terrain, its surrounding ring, static
  meshes (culled by `renderer.contributionCull`), roads and overgrowth (to its own range); water
  over the whole map, then sea cells to the horizon wherever the ring ends in open sea. Vehicles,
  soldiers, particles and decals end at the limit. The land's shape is visible further, targets
  are not (accepted by the owner).
- Full haze is the sky's radiance along the ray. With the realistic haze, the haze no longer hides
  the cut: at Muttrah City's 700 m a level ray keeps about 70 % of its contrast.

**Shared lighting model** (contract 11, shaders outside this tree). Every per-pixel lit receiver
takes the sky as the SH at its normal times its sky visibility: statics and terrain the
lightmap's, vehicles and soldiers the hemi map's alpha faded out with height over the terrain,
first-person arms the open sky. Foliage lights the sun per vertex and the sky per pixel at the
vertex normal; particles read the SH in their vertex shaders (`RaSkyShVertex`): mesh particles at
their normal, billboards and trails averaged. Undergrowth and roads, which have no normal, take it
facing up. One perceptual roughness and F0 drive the sun's GGX highlight and the sky's split-sum
reflection, occluded by the sky visibility (Lagarde); roughness widens with the normal's
screen-space change (the normal maps' mips are renormalized, so Toksvig has nothing to read).
Material classes are picked in the shaders, not the engine: bundled meshes by technique and
flags (paint, glass, matte), soldiers' gear (paint with an environment map, else fabric), static
meshes by the detail map's gloss (PR's old specular mask: stone and plaster stay matte, metal and
glass near 1 go glass-sharp). Retail environment maps are no longer sampled or blended into
albedo; the engine still loads and binds them (the terrain's environment-mapped layer reads its
manager).

**Water** (contract 12). Screen-space reflections: just before water draws, above water, the scene
is copied to a single-sample texture (s12), and water marches against the prepass's linear depth,
falling back to the sky cube where the ray leaves the screen. Fresnel at water's F0 weighs the
reflection against the water body, lit by the sun and the sky with the authored water colour as
albedo. Seen from under water, water stays as it was.

**Cost** (1920x1080, 4x MSAA, 2026-11-22, same session against the milestone's switches off; every
frame CPU-bound): Muttrah City `bench1` 8.07 → 9.20 ms, Fallujah West `bench` 14.55 → 16.00 ms,
Saaremaa `bench1` 5.13 → 5.78 ms. Over the 1 ms budget on two paths, accepted by the owner
(2026-11-22). The view distance's scenery is most of it (0.91, 1.24 and 0.43 ms: about 220 more
draws on Muttrah City); the atmosphere 0.1–0.2 ms, water's reflections 0.04 ms.

Left, outside the milestone:
- Stars and the night sky: the night layer (The Falklands, `gpm_cq 32`) loads and lights as before,
  but the painted dome's stars and night clouds are gone (owner, 2026-11-22: fine until milestone
  12).
- Light shafts (dropped for now, owner 2026-11-21); thermals and under water keep their own looks.
- Point lights, baked and dynamic, not checked against the new sun.
- Decals' sky is not occluded (the hemi map holds only the ground's sky visibility); first-person
  models take the open sky rather than the visibility under the camera; weapons' metal and fabric
  under colour-map gloss have no class of their own.
- View distance: the far ground ring (milestone 17; land past the surrounding ring shows sky; below the horizon
  the sky repeats its horizon row), vegetation past overgrowth's range, jets and anti-air not
  checked, the far scenery's cost.
- Banding in the dark blue and the sun's glare: milestone 13 (dither, bloom).

### 15. Shader pipeline

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

### 12. Clouds

Decided by the owner, 2026-11-20: the levels' painted cloud layers are replaced by modern clouds,
not constrained by the game's approach; the sky is clear until then.

**Reachable clouds** (owner, 2026-11-22, replacing the rule of 2026-11-20 that clouds stay at
infinity and hide nothing): with Dagor's structure the clouds are volumes in the flight space,
flown under, into and above, and they may hide aircraft and the ground. The GPU budget is set
later (owner); until then it is measured, not capped.

- **Volumetric clouds.** Low and mid cloud in baked volumes (structure after Gaijin's Dagor
  daSkies2, below): Perlin-Worley shape and erosion noise, the cloud light table and sky as
  ambient, aerial perspective. Near clouds are marched from the camera per screen pixel at reduced
  resolution with temporal reprojection, stopped at the scene's depth; far ones come from a
  cloud panorama (2048x1024, read with a Catmull-Rom filter; the 192x108 sky-view table is too coarse for cloud edges),
  updated over several frames, drifting with the wind. Each panorama is blended into the previous
  one (weight 0.25) with a march offset that moves per panorama, so its jitter averages out
  instead of showing as grain about five screen pixels across, changing every 8 frames. The marches are real loops: unrolled, the
  panorama compiled for 17 s on the main thread, and the loop draws the same under DXVK (the
  load-time tables' loops did not).
- **Coverage drives the lighting.** One coverage value per level (0 clear, 1 storm; picked per
  level outside this tree, a default otherwise) sets the clouds and the light together: the sun
  dimmed and its shadows softened, sky light greyer, more even and relatively brighter, the haze and
  reflections greyer through the sky they read. Overcast reads through the lighting, not the
  texture.
- **Low scud (perhaps, later).** The volumes now give parallax and fly-through, which the planned
  sparse particle clouds were for; ragged scud under an overcast deck, finer than the 128 m field,
  is left as an option once the volumes are judged.
- Cloud shadows on the ground (a 2D map from the sun, scrolling with the wind) and into the sky
  cube.

- Stars and the night sky (owner, 2026-11-22): the painted dome's stars and night clouds went with
  milestone 11; the night layers get them back here.

Planned (owner, 2026-11-22), from a survey of the literature (below):

- **Physics sets the structure, noise the detail.** Real cloud behaviour enters through a small
  atmosphere per climate preset (hot desert, temperate maritime, more as levels need them): surface
  temperature and dew point give the cumulus base (lifting condensation level, about
  125 m × (T − Td)), the inversion height caps the tops, stability picks flat cumulus over towering
  ones, wind and shear orient streets and cirrus fall streaks. Genera sit in slabs at their WMO
  altitudes (low Cu/Sc/St, mid Ac/As, high Ci/Cc/Cs). A 2D weather map per slab (coverage, genus,
  height) feeds Perlin-Worley shape and erosion noise; its statistics are checked against real
  clouds (power-law cloud sizes, a fractal perimeter).
- **Coverage stays an explicit value per level**, not derived from humidity (real cover follows
  large-scale weather a level does not model). The preset says which clouds; coverage says how many.
- **Clouds evolve, cheaply:** each weather-map cell has its own life phase (growth and decay over
  10–30 min) on top of the wind drift, so a round does not watch one texture slide by. Coverage and
  time of day are constant for the round.
- **Light transport in physical units.** Density is liquid water content (g/m³); extinction
  3·LWC / (2·ρ_water·r_eff), about 0.03–0.15 m⁻¹ for cumulus. The phase function is Jendersie and
  d'Eon's Henyey-Greenstein + Draine fit to Mie for a droplet diameter (10–20 µm), replacing the
  dual-lobe phase; multiple scattering by octaves (Wrenninge; Hillaire) replaces the powder term,
  since clouds are optically thick and most of their light is multiply scattered. One shader include
  holds this cloud material, shared later by the 3D particle clouds.
- **Baked volumes, marched cheaply (from Dagor).** Gaijin's open-source Dagor engine (BSD-3-Clause,
  `prog/gameLibs/daSkies2`; War Thunder) marches a baked density field instead of evaluating noise
  per step: a 512x512x64 one-byte field over a 65 km tiled weather world (about 128 m per texel),
  regenerated over frames, an ⅛ copy to skip empty space, and a 256x256x64 two-channel volume of sun
  transmittance (along the whole path to the sun) and ambient occlusion, updated in chunks. A step
  costs one density, one light and (near) one detail-noise fetch, against about 15–25 in a
  noise-per-step march with a short sun march; the light volume also darkens cloud bases under
  the cloud above, which a short sun march misses. Dagor needs D3D10+ (it renders into volume
  slices; no D3D9 driver). Here, in D3D9 form: both volumes as 2D atlases of their slices
  (4096x4096 one-byte density, 16 MB; the light volume in G16R16), rendered by pixel shaders over
  several frames and sampled with two fetches and a manual blend between slices; the panorama in
  A16B16G16R16F. Detail erosion noise is added at sample time (128 m texels are coarse for a deck
  1–2 km overhead). The 65 km tiling may show toward the horizon (a ray through a deck at 2 km
  runs over 100 km): checked in the prototype, a coarser far field if it does. Dagor's droplet phase
  (`clouds_droplet_phase.hlsli`) and octave setup can be ported with attribution.
- **High cloud as a flat layer.** Cirrus, cirrocumulus and cirrostratus are too thin to shadow
  themselves and finer than a 128 m field holds: like Dagor's strata layer (`strata_clouds.dshl`),
  they are a procedural layer at their altitude (streaks along the wind and shear), drawn into the
  same panorama with the same phase function and sky light. The volumes carry low and mid cloud.
- **Also from Dagor:** light colour at the cloud's altitude (high cloud stays lit after ground
  sunset; tops warmer than bases), a continuous cloud type for the height profiles, the haze dimmed
  by the sun ray's length through the cloud slab, the panorama crossfaded between two updates and
  blended in tonemapped space, Beer shadow maps for ground shadows, and stars and moon placed by
  latitude and sidereal time; with reachable clouds (2026-11-22) also the camera-traced near
  clouds with temporal reprojection and the panorama for the far field. Not taken: the clear-hole
  logic, the rain map (perhaps with weather later), node-graph generation.
- **Not done:** a coarse atmospheric simulation at load (Goswami and Neyret 2017; Vimont et al.
  2020) writing the weather map. Its gain is clouds tied to terrain (sea breezes, ridge caps), which
  a sky at infinity does not show; reconsidered only if a mountain level looks wrong. Full cloud
  fluid simulation (Stormscapes 2020, Weatherscapes 2021, Nubis³ 2023) is out of reach of D3D9 and
  wasted without parallax.

**Tuned by eye** (owner, 2026-11-22): the planned reference set (about 8 Poly Haven HDRIs, their
luminance ratios and cloud statistics) is dropped; the clouds are tweaked by eye in game.

The prototype (the two presets, the coverage value, the Mie phase and multiple scattering, before
the lighting is coupled) is judged on Kashan Desert from the ground, the air and inside a cloud at
coverage 0.3, 0.7 and 1.0, by eye. Its GPU time at
1920x1080 is measured on the benchmark paths; the budget is the owner's to set (the first plan's
0.3–0.6 ms was for the panorama alone).

**Built so far.**
- *Task 1, the cloud material* (`RaCloud.fxh`, shaders outside this tree). Density
  in g/m³ of liquid water; extinction 3·LWC/D (D the droplet diameter in µm). Phase: Jendersie and
  d'Eon's fit, delta-scaled: its HG part (g ≈ 0.995, under 0.1° wide, half the extinction:
  the diffraction peak) is treated as unscattered, so extinction is times the Draine weight w
  (0.50 at 20 µm) and the phase is the Draine lobe alone, evaluated directly (normalized, 1/sr).
  Dagor keeps the spike, which would put about 2700/sr into a sub-pixel ring around the sun.
  Multiple scattering: Dagor's three octaves (contribution 0.7, attenuation 0.3, eccentricity
  0.6, weights normalized to 1); the first octave takes the droplet phase, the others a dual-lobe
  HG; the light volume will store the last octave's transmittance and the include raises it to the
  others'. Ambient: the sky from above, half desaturated (Frostbite, Dagor), and the ground from
  below, averaged over the sphere. A test effect compiles to 113 ps_3_0 slots.
- *Cloud light* (`Atmosphere.cpp`, `buildCloudLight`, after Dagor's `clouds_light_color`),
  built with the fit (adds about 25 ms): `RaCloudLightTable`, 64x24 `A16B16G16R16F`, managed. 8
  altitude tiles across (layer i at (i + 0.5)/8 of 12 km above the reference height), 3 rows of
  tiles down: the sun's irradiance (the sun above the atmosphere times the transmittance there),
  the sky's mean radiance from above (its cosine-weighted irradiance / π, from the sky model at
  that radius and sun cosine) and the ground's radiance from below (albedo times the ground's sun
  and sky irradiance / π, through the air up to the layer). A tile's 8x8 texels span ±280 km
  around the level's origin, so the curvature lowers a far deck's sun. The haze is left out, and
  the ground everywhere has the level's albedo. Kashan Desert (sun 50°): sun 7.3/7.4/7.1 at
  0.75 km to 7.7/8.1/8.9 at 11 km; sky 0.07/0.15/0.36 to 0.02/0.04/0.12; ground 0.59/0.49/0.30.
  `tools.atmosphere` dumps it (`[cloudlight]`). Nothing samples it before task 3.
- *Weather and volumes* (`Clouds.cpp`, contract 13; Dagor's structure in D3D9 form). A weather
  map, 128² A8R8G8B8 over a 65,536 m world that wraps (r low layer's coverage, g mid layer's,
  b cloud type: 0 stratus, 0.5 cumulus, 1 cumulonimbus), built on a worker thread from Dagor's
  `gen_weather` (28 ms; Perlin octave sums unnormalized, as Dagor's thresholds expect). Each
  region (~2 km) of the low and mid signals swells and shrinks by its own phase and period
  (10–30 min) around its mean, so coverage holds while clouds evolve. Noise volumes (built once on
  worker threads, ~190 ms; managed L8): shape 128²×32, Dagor's Perlin-Worley at five Perlin
  octaves (six and seven alias at 128 texels), and detail 32³ Worley for task 3's erosion. Built
  by pixel passes into a double buffer and made live whole (about 23 frames: the weather, 8
  frames of field, 1 coarse, 8 light; again every 10 s, or at once when a setting changes):
  - field, 512²×64 density over the world and the slab (128 m across), a 2048² A8R8G8B8 atlas of
    4×4 tiles, four slices per texel in rgba. Dagor's density with its cloud-type height profiles
    (low layer the lower 80 % of the slab, mid layer the top third, bottom rounding, the
    cumulonimbus waist and anvil) and its shape tiling (each 4 km cell its own offset and
    rotation); not taken: the curl-noise turbulence, the rain map. Read with two fetches and a
    blend between slices (`Ra_CloudFieldDensity`).
  - coarse, the field's maximum over 8×8×8 texels, 128×64 A8R8G8B8 (2 tiles of 4 slice groups),
    for task 3 to skip empty space; the dump checks it bounds the field.
  - light, 256²×64 A16B16G16R16F, 8×8 tiles: the transmittance toward the sun at the last
    octave's extinction (32 steps, quadratic, to the slab's top or 30 km), and the octave-weighted
    visibility straight up and straight down (8 steps each), which weigh the sky's and the
    ground's light in `Ra_CloudAmbient`.
  Two layers, each with its own heights (Dagor's `CloudsForm` layers; the slab spans both, and
  each layer's height fraction is mapped from the slab's): a low one (cumulus, its type from the
  weather map) and a mid one (stratus-type sheet), the mid one with its own coverage map and
  density. Presets (`renderer.cloudPreset`): *desert* low 2–6 km above the reference height (hot,
  dry: base 125 m × (T − Td) ≈ 2 km; 3.5 km until 2026-11-22, too high for 4 km maps by eye),
  type 0.2–0.6 (small cumulus to towers), mid 6.5–8 km at density 0.6 and 0.8 of the coverage,
  LWC 0.6 g/m³, droplets 12 µm (continental); *maritime* low 0.5–4.5 km, type 0–0.3 (stratus to
  stratocumulus), mid 3.8–5.5 km at 0.35 and 0.6 of the coverage, 0.35 g/m³, 20 µm.
  `renderer.cloudCoverage` (default 0.4) feeds Dagor's coverage curve. `renderer.cloudBase`,
  `cloudTop`, `cloudMidBase`, `cloudMidTop` (m above the reference height, 0 the preset's),
  `cloudMidDensity` and `cloudMidCoverage` (−1 the preset's) override them. Measured on the
  3.5–9.5 km single slab before then: Kashan Desert, desert preset, from above: the share of the sky with column
  optical depth over 1 is 0.14, 0.72 and 0.95 at coverage 0.3, 0.7 and 1.0 (seen from the
  ground towers' sides add to it); the clouds sit 3.6–7 km. Open, for the panorama (task 3) to
  show: the field's shapes read as fuzz from above rather than distinct cumulus cells, and at
  low coverage they bunch in one ~20 km weather region (Dagor's weather frequency, 2 per world).
  `tools.clouds` dumps every volume.
- *The panorama* (`Clouds.cpp`, `RaClouds.fx` technique `RaCloudPanorama`, `RaCloudPanorama.fxh`,
  contracts 14–15), the far field since task 4. 2048x1024 `A16B16G16R16F` over the whole sphere
  seen from the camera's position, latched at a panorama's first band, past the handoff distance
  (`renderer.cloudNear`): u the azimuth, v 0.5 + 0.5 sign(e) sqrt(|e| / 90°) for the elevation e
  (row 0 the nadir, the horizon halfway), finer toward the horizon. Below the horizon it holds
  clouds only while the camera is inside or above the slab (`physics.airDensityZeroAtHeight` is
  10 km in PR, so jets reach both presets' slabs). rgb the light the clouds and the air in front of them add, stored as
  rgb / (1 + luminance); a the clouds' transmittance. Drawn a band of 128 rows a frame into a ring
  of three, each from the volume set and time latched at its first band (a build waits rather
  than overwrite the set a panorama reads); the sky (`RaSky`) blends the previous and the newest
  by the next one's progress, so a panorama fades in over 8 frames, in the compressed space, and
  multiplies the sky and the sun disc by a before adding rgb.
  - March: on spheres of the planet's radius, through the slab from below, inside or above it
    (`Ra_CloudShell`), from the handoff (or the slab, if later) to where the ray leaves it or 150 km; steps of
    100 m or 1.5 % of the distance, whichever is longer (about 60 overhead, 90 at the horizon, at
    most 128), from a white-noise offset per texel and panorama, until the transmittance is under
    0.01. A step reads the coarse copy, and only where it bounds cloud the field, the detail noise
    and the light volume. Even steps with an interleaved-gradient offset (the first build) left
    a regular stripe pattern in the far deck. The cloud light table is read at the segment's two
    ends and blended. Light per step as `RaCloud.fxh`.
  - Detail: the 32³ Worley at a 1.5 km tile erodes edges only, density − 0.2 · detail ·
    (1 − density), full to 8 km and none past 25 km (its 47 m texels alias further off).
    Subtracting 0.4 · detail outright first erased most of the field (mean densities are 0.1–0.3)
    and left coverage 1.0 at 61 % cover overhead; now 89 % (0.95 from above, where the towers'
    sides do not count).
  - High cloud: one sample where the ray meets its sphere (desert 11 km above the reference
    height, maritime 8 km): patches from the shape noise at 40 km, thresholded by the preset's
    high coverage (0.35, 0.5), times streaks of the shape noise stretched 10:1 along the wind and
    3 km across; vertical optical depth 0.3 / 0.4 at full density, over the cosine at the layer;
    lit by the octave phase with the sun through half its depth, plus the ambient. Independent of
    `renderer.cloudCoverage`. A single threshold of two octaves first gave short flecks over the
    whole sky.
  - Aerial perspective per layer at its transmittance-weighted mean distance: the air's Rayleigh
    and Mie optical depth from the observer in closed form for an exponential layer along the
    ray's rise; the air's in-scatter in front is the sky-view radiance times
    (1 − Ta(d)) / (1 − Ta(∞)), Ta(∞) with a spherical layer's air mass. The ground haze is left
    out (the sky is not hazed either).
  - Wind: desert 8 m/s, maritime 12 m/s (directions per preset), drifting the field under the
    panorama; the weather's own evolution is in the field's frame.
  - `renderer.clouds` (default 1): 0 stops the builds and the panorama, and the sky is clear.
    `tools.clouds` adds the decoded panorama and its transmittance as images and, per band of
    elevation, the mean transmittance, the share under 0.5 and the mean added luminance.
  - Kashan Desert (agent, 1920x1080): at 0.3 a few far banks and cirrus; at 0.4 cumulus with
    whiter sunlit sides; at 1.0 a broken deck. Open for task 5: the cumulus may read greyer than
    real ones (the octaves' energy, judged against the references), the low-coverage clouds still
    bunch in one weather region, and the horizon under overcast keeps the clear sky's colour
    (rays below about 1.2° meet the slab past 150 km, where the air hides it; task 6 greys the
    sky). Not measured: its GPU cost (the final GPU pass).
- *Near clouds* (`Clouds.cpp`, `rendererCloudsNear`; `RaClouds.fx` techniques `RaCloudNear`,
  `RaCloudNearResolve`, `RaCloudNearComposite`; contract 15). After the sky, before the world's
  transparents (Frame item 5; GPU span `clouds`), three passes:
  - March, at half the view's size (`A16B16G16R16F` light and transmittance; a second target with
    the distance to reproject by and the scene depth used): from the camera to the handoff or the
    farthest of the texel's 2x2 linear depths (s15), whichever is nearer, through the slab by the
    panorama's march (`Ra_CloudMarch`, shared): steps of 25 m or 2 % of the distance, twice that
    through coarse blocks without cloud, at most 128; the offset is interleaved gradient noise
    moved by the golden ratio each frame. White noise and 4 % steps first left grain the history
    did not settle. A second detail octave six times finer (250 m tile) joins inside 3 km. Aerial
    perspective as the panorama's, from the camera's height.
  - Resolve into a history (two, ping-pong): the previous one at where the texel's clouds were last
    frame (along the ray at their mean distance, back by the wind's drift since then, through the
    previous view), clamped to the current 3x3 range, blended 0.9 / 0.1. Invalid after a skipped
    frame, a resize or a reset.
  - Composite per scene pixel, blending ONE, SRCALPHA: the four nearest resolved texels, weighted
    bilinear times 1 / (0.01 + their depth's relative difference to the pixel's).
  Skipped under water and in thermal views (fog colour with no red, as `Ra_IsTisActive`).
  `renderer.cloudNear` (default 16000 m, 0–40000; 0 none: the panorama from the camera, behind the
  scene only). `tools.clouds` adds the last resolve (`-near.png`, `-near-t.png`) and its mean
  transmittance.
  Kashan Desert (agent, 1920x1080, views `m12t4-*` at 3, 4.5 and 8 km): from the ground the near
  and far fields join without a seam; from 8 km the deck lies over the hazed ground, its sides
  toward the sun shaded and warmed by the ground's light; at coverage 1.0 from 4.5 km the camera
  sits in fog with cloud walls around. Open: the world's transparents (particles, smoke) still draw
  over clouds in front of them; the ground from altitude is the haze's flat colour past fog end
  and the map a dark slab (milestone 11's view distance, not the clouds); close up the 128 m field
  reads as soft fog. From altitude (Kashan Desert, 2026-11-23, low sun) a circular step at the
  handoff: the near field takes the aerial atlas (ground haze in), the panorama the closed form
  without the ground haze and coarser steps, and they switch at one distance; to fix, the panorama
  takes the atlas where it reaches and the two cross-fade over a band. Not measured: the GPU cost (the final GPU pass).
- *Coverage drives the lighting* (task 7; `SkyCube.cpp`, `Atmosphere.cpp`, `Clouds.cpp`; contract 16).
  A 256x128 panorama of the clouds from the level's origin, 2 m over the reference height
  (`RaCloudPanorama` into a small target, no history), is drawn about every 30 s and when a set
  with new settings goes live, read back two frames later and handed to the atmosphere. The sky
  cube and its SH are rebuilt from it on a worker thread (about 90 ms; the last cube stays bound
  meanwhile): above the horizon the clear sky times the clouds' transmittance plus their light,
  the ground in it lit by the sun times S, the clouds' mean transmittance toward the sun's
  elevation (all azimuths, ±3°). So overcast greys and evens the sky light and reflections of every
  surface. The air under the clouds takes `Ra_HazeLight`: the clear sky's radiance times S plus
  (1 − S) times the clouded sky's mean radiance (`RaAtmosphere[8]`, Dagor's statistical clouds
  shadow): the haze, the air in front of the clouds (panorama, near clouds; only while the camera
  is under the slab's top), the sky pass within about 3° of the horizon, and the clouds' light from
  the ground below them. Past the panorama's 150 km reach the slab's clouds are extrapolated from
  the marched part (to 400 km), so an overcast deck closes to the horizon. The sun on surfaces is
  dimmed by the cloud shadow map (task 8), not by S, which takes over only past the map.
  Kashan Desert (agent): at 0.4 S 0.99 and the sky light as clear; at 1.0 (desert preset, a deck of
  towers 2–6 km) S 0.12–0.14, sky irradiance up 0.24/0.25/0.29 against the clear 0.09/0.18/0.39,
  global horizontal about 17 % of the clear day's, the diffuse part 24 %. Against references:
  the CIE overcast sky (horizon about a third of the zenith) is matched in shape, the horizon
  darker than overhead; Kasten–Czeplak puts an 8/8 deck at about 25 % of clear and mostly diffuse,
  so this coverage 1.0 reads as a storm deck (towers with optical depths in the hundreds), as the
  coverage scale intends, darker and more direct than a stratus overcast. `renderer.clouds 0`
  restores the clear sky's light (checked: the same irradiance as before).
- *Cloud shadows* (task 8; `Clouds.cpp` `shadowStep`, `RaClouds.fx` technique `RaCloudShadow`,
  `RaSunShadow.fxh`; contract 16). A Beer shadow map (Hillaire 2020), 1024² `A16B16G16R16F` over
  32 km of the plane at the reference height, centred where the camera's sun ray meets it and
  snapped to its 32 m texels: per texel the optical depth toward the sun through the slab (48 steps,
  field and detail erosion at full strength, delta-scaled extinction), and the world heights where
  the ray enters and leaves cloud. Double-buffered, drawn a band of 128 rows a frame and restarted
  a second after the last; the wind's drift since it was drawn goes into the receivers' constants.
  Receivers move along the sun ray to the plane and take exp(−τ × the share above them), so
  aircraft in or above the deck are lit right; the outer tenth fades to S. Every sun receiver
  takes it: the shared resolves (statics, bundled and skinned meshes, trees, terrain, roads,
  decals), water, undergrowth, and particles per vertex. Kashan Desert at 1.0: mean
  transmittance 0.11 (S 0.08–0.14), 90 % of the ground under 0.5. Open: the first-person passes
  (RASM zero) take no cloud shadow.

### 16. Water

Decided by the owner, 2026-11-22, with milestone 12 paused: the water shader is rewritten, not
patched. What the owner sees on The Falklands: one pattern tiled uniformly (a single 3D normal
volume repeating every ~30 m at one scale), flat and low in detail up close, a ring around the
camera (task 1), and streaks in the reflections (task 1). Kept: the water's geometry and height
(physics and boats float on a flat plane), the under-water look and thermals, the reflections'
contract 12. The levels' water settings (`WaterColor`, `WaterScroll`, the lightmap's water depth)
become inputs: the colour an absorption hint, the scroll the wind's direction and speed.

- **Waves from a spectrum.** An ocean spectrum (JONSWAP, Tessendorf's FFT) evolved on the GPU each
  frame into three tiling cascades whose sizes do not divide each other (about 500, 90 and 17 m,
  256² each), summed by the shader so no period is visible; slopes, horizontal displacement and its
  Jacobian (for foam). Wind and fetch per level, a default otherwise.
- **Filtered normals and a sun path.** Each cascade's slope variance kept through its mips (LEAN
  mapping) widens a GGX sun lobe as the waves shrink below a pixel: near water shows the waves, far
  water a smooth glitter path, no aliasing. Replaces the retail Blinn highlight.
- **The water body.** Refraction of the scene copy (s12) offset by the waves, the water's depth from
  the scene's linear depth (s15), Beer absorption and in-scatter tinted by the level's colour
  (replacing the lightmap's depth and the retail opacity), light through the wave crests facing
  the sun.
- **Foam.** At shores from the water's depth, on crests from the Jacobian; noise-broken, never a
  tiled texture.
- **Geometry: normals only** (owner, 2026-11-22). Displacing a camera-centred grid by the spectrum
  would give real wave shapes, but boats, physics and the shore are on a flat plane; left for later.

Tasks (in [STATUS.md](../STATUS.md)) closed with the owner's look at The Falklands and Saaremaa
(2026-11-22); the GPU cost waits for the final GPU pass.

As built:
- *The ring* (task 1). Past the sun cascades water took the terrain lightmap's g as its sun
  visibility, but g is visibility × N.L (`Ra_ResolveSunCosine`), 0.59 on flat water under The
  Falklands' sun at 36°: the sun's highlight and the body dropped where the cascades end. Water
  now divides the sun's height out (`RASM[17].y`, floored at 0.05).
- *Reflection streaks* (task 1). The march took a hit wherever a point lay behind the scene within
  `2 × step + 2 %` of its depth, tens of metres far out, so rays passing behind masts and poles
  hit them and smeared them down the water. A hit now needs the step to have crossed the scene's
  depth (thickness the step's own depth span, at least 5 cm + 0.2 % of the depth).
- *Waves* (task 2; `Water.cpp`, `RaWater.fx`; contract 17). JONSWAP with Mitsuyasu's spreading,
  built on a worker whenever the wind changes (32 ms) into h0 for three cascades of 256² over 503,
  87 and 17.3 m, each holding wavenumbers from 6 of its fundamentals up to 6 of the next one's.
  Each frame: the spectrum at t (frequencies rounded to whole cycles of 1000 s, so t wraps without a
  jump), 16 Stockham passes over a 768 x 256 `A32B32G32R32F` pair, then each cascade's slopes and
  their squares into its mipmapped map (mips by `StretchRect`); 20 draws and 24 blits. The water
  shader sums the three slopes into its normal; retail's tiled normal volume (`WaterMap`,
  `WaterMapFrame0`/`1`) is no longer read, and before the first frame the water is a flat plane.
  `renderer.waterWind` (7 m/s), `renderer.waterFetch` (60 km); the wind's direction is
  the level's `WaterScroll`, +x when it has none. The Falklands at the defaults: significant height
  1.17 m, mean square slope 0.026. Geometry stays flat (owner, 2026-11-22: normals only for now).
- *Filtered normals and the sun path* (task 3). The slope maps hold the slopes along and across the
  wind (the level's `WaterScroll`, which the water shader reads too) and their squares: in that
  frame the spectrum is symmetric and the slopes' covariance zero, so four channels hold the
  whole second moment. LEAN mapping: the filtered means summed are the mean slope (the normal),
  each cascade's squares minus its mean squared the variance its footprint averaged away. The
  finest cascade also carries the variance of the waves below its grid (13.5 cm), what tops the
  resolved variance up to Cox and Munk's glitter statistics (σu² = 0.00316 U, σc² = 0.003 +
  0.00192 U; at least a quarter of them): 0.0078 and 0.0044 at 7 m/s on top of 0.0143 and 0.0121.
  The sun's highlight is anisotropic GGX over the slopes about the flat surface, centred on the
  mean slope, α² = 2σ², height-correlated Smith masking, Schlick's Fresnel at F0 0.02, times the
  sun's irradiance (`Ra_SunIrradiance`, as every surface) and visibility; the retail Blinn
  highlight with the level's `SpecularColor` stays only under water.
- *Rough reflections* (task 3, owner 2026-11-22: keep them physical). The same variance is the
  reflection's roughness (α² = 2σ², the two axes' geometric mean): the sky cube read at its mip,
  the screen-space hit read from the scene copy's mip whose texels are the reflected cone's width
  where it meets the scene (2 × distance × α), and the reflected fraction from the split-sum table
  (Fresnel and masking over the rough surface) in place of the mirror's Schlick. The waves tilt
  the reflection fully (retail's normals keep half). At grazing the reflected fraction falls from
  about 0.7 to 0.3, as Bruneton's mean Fresnel for the ocean does; the retail body colour then
  fills the far sea, green and too bright, until task 4 replaces it.
- *The water body* (task 4). Above water (the scene copy bound) the surface draws opaque: the
  scene under it is read from the copy where `refract(-V, N, 1/1.333)` meets the water's
  thickness (the opaque linear depth minus the surface's, along the ray; unbent where the bent read
  lands on something in front), through a medium of Beer extinction and single scattering. The
  level's `WaterColor` is deep water's colour: albedo = its linear value × 8 (deep in-scatter,
  albedo / 4 of the light, then about the retail body's colour times the light), extinction
  0.03 / albedo per metre (The Falklands: 0.25, 0.13, 0.25). The bed's light is attenuated on its
  way down too (along the refracted sun; sky light at 1.2 m per metre); in-scatter of the refracted
  sun's beam and the sky, each falling off with depth, integrated in closed form; a crest term
  (the sun through the backs of the waves facing the eye when looking toward a low sun). The
  copy's air in-scatter is taken off before the water attenuates it and added back once with the
  surface's. The seabed is drawn only in patches near land; the sky behind the water, and any bed
  within a pixel of it (the patches' resolved edges are half sky), is deep water. Replaces the
  retail opacity, the lightmap's water depth and the retail body colour; under water nothing
  changes.
- *Foam* (task 5; contract 18). The FFT atlas has a fourth block of columns, the cross derivative
  Jxz of all three cascades, so each has its whole Jacobian of the displaced surface (x + λD,
  D = i k̂ h toward the crests, λ 1). `RaWaterFoam` per foam cascade (87 and 17.3 m; the 503 m one
  is swell and does not break): the compression 1 − J past a threshold ramps in fresh foam over
  0.5 σ, last frame's fades by 1/e in 3 s; 256² `G16R16F` maps with mips (r the foam, g the
  compression in ramps), ping-ponged, on s4–s5. To
  first order the compression is the displacement's divergence, of variance the cascade's mean
  square slope σ²; the threshold is the Gaussian quantile at which whitecaps start on Monahan's
  coverage (3.84e-6 U^3.41) / (4 × 2) of each cascade, the 4 the fade's spread. Measured from the
  maps' last mips (logged once, 30 s after a spectrum): 4.97 % at 15 m/s against Monahan's 3.93 %.
  The surface shader combines the two cascades and the shore as independent layers. The 17.3 m
  cascade's foam is weighted by the 87 m cascade's compression (1 + c/σ, clamped: short waves break
  on the longer ones' crests), so its period does not show as a tile (owner, 2026-11-22); the 87 m
  cascade's by a 40 m value-noise field of mean 1 drifting downwind. The shore's share is
  1 − depth / (Hs / 0.78), the depth waves break at, the depth to the bed along the unbent view
  ray (the refracted one jitters with the waves), deep where sky is a pixel away; the ramp is
  box-filtered over the depths the pixel spans (`fwidth`), from the surface down, so a band
  narrower than a pixel fades instead of stepping. The coverage is drawn as
  two octaves of noise (1.6 and 0.35 m, three times longer downwind) below it, translucent as it
  thins; past 1.6 m of pixel footprint the plain coverage. Foam is a diffuse layer of albedo 0.6
  under the sun (the wave normal's N.L, shadowed) and the sky, over everything else. What shows is
  less than the maps' coverage (the fraying and the groups' clamp take some off).
  `renderer.waterFoam` scales the coverage (1; 0 no foam maps). `RaWaterFoam` (c188): x 1 while the
  maps are bound, y the breaking depth, z the waves' loop seconds.
- Water's shaders (`RaWater`, the water material) are not rebuilt by `rdev.sh reload`; a level load
  picks them up.

### 13. HDR bloom + auto-exposure

Accepted by the owner, 2026-11-23, by eye; no GPU timing (owner). Task 6 dropped. Everything runs in
`HdrPost.cpp` (effect `Shaders/RaHdrPost`, contract 19), called by the post-production composite
(`postProductionFinish`) on the resolved single-sample float scene just before the final copy
(FSQuadDrawer pass 0, the grade), after tinnitus, flashbang and gas mask; skipped when TV draws
straight to the back buffer. Thermals (fog colour with no red) skip all of it: exposure 1, no bloom,
the 2.2 encode, as before. Under water it all applies. Every pass is a full-screen pixel pass into
a target of its own, unrolled by hand (no shader loops: they came out wrong under DXVK).

1. **Dither and the encode.** The grade adds triangular noise of one 8-bit step (two interleaved
   gradient noise values, offset each frame) after the encode. The encode is the sRGB curve (the
   curve the samplers decode colour textures with), darker than retail's 2.2 power below 0.0031
   linear; thermals keep the 2.2 power. Tonemap and encode are now separate (`RGrade_Apply`, `RGrade_Encode`).
2. **Scene luminance.** The bloom's down chain runs (to its fourth level, 1/16 of the scene) even
   with the bloom off; that level, bilinear, down to 64 x 32 log2 luminances with a centre weight
   (1 at the centre to 0.25 at the corners). The 64-bin histogram over −14 to +10 EV is a gather,
   not the planned point scatter (no vertex buffer, no float blending): 16 x 64 partial sums (each
   texel half a meter row, four bins in rgba), then 16 x 1 (`A32B32G32R32F`). Metered value: the
   mean EV of the bins between the 50th and 95th percentile of the weight.
3. **Auto-exposure.** A 1 x 1 `A32B32G32R32F` ping-pong: adapted EV, exposure (multiplier),
   metered EV, 1 once written. The adapted EV moves toward the metered one by 1 − e^(−dt·rate),
   3/s into light and 1/s into dark, times `renderer.exposureSpeed`; it snaps on the first frame
   after a device reset and after a frame gap over 0.5 s (a level load). Exposure in stops =
   log2(key / adapted) + `renderer.exposureBias`, clamped to `renderer.exposureMin`/`Max`
   (−4, +4). The key is 0.42, calibrated, not 0.18: Muttrah City's reference views meter −1.0 to
   −1.3 EV, and 0.42 (−1.25 EV) shows them at milestone 11's accepted look (0 stops). The ceiling
   is what keeps night dark (task 5). `renderer.exposureBias` adds stops over it.
   Particle emissives read the state per vertex (`RaExposureParticle`, `RaExposureState`) and
   divide by the exposure, so an emissive tint reaches the tonemap at its authored value, as
   retail showed it, by day and by night; one frame late (the state is written at the frame's end).
4. **Bloom.** Six half-size `A16B16G16R16F` levels from the scene (level 0 half its size); down
   with Jimenez's 13-tap filter, level 0 Karis-averaged (each of its five boxes weighted by
   1 / (1 + luminance)) and every scene sample's luminance capped at 100; up with a 3x3 tent of
   one smaller-level texel, added (blend ONE, ONE) into the level above; the grade takes level 0's
   tent at full size, divided by the level count, and mixes it in before the exposure:
   lerp(scene, bloom, `renderer.bloomStrength` 0.04). Outside thermals retail's glow is off: `postProductionSetGlowEnabled` keeps the owner's glow byte 0,
   so neither the scene glow (`GlowMaterial`) nor the `RSMTGlow` queue nor their composite runs,
   as with retail's glow off; the night-vision distortion skips its glow part. Thermals, which
   skip the HDR post, keep retail's glow until their rewrite. Facing the sun the
   disc gets a small halo, nothing tuned per source.
5. **Night.** The Falklands' night layer (`gpm_cq 32`, view `w0`) meters −9 EV and sits on the
   ceiling: at +4 stops its terrain draws at about 25/255 (2.2 encode at 0 stops: 8/255; retail
   about 30/255, reached at +4.5, where the scene starts to read as an overcast dusk). The toe is
   not changed. Accepted at +4.
6. **Compressed (Karis) resolve: dropped** (owner, 2026-11-22). The owner sees no aliasing at
   bright edges with auto-exposure and bloom on. It would have meant every scene pixel shader
   writing compressed colour through an engine-owned register flag, an expansion pass at the
   resolve, and every pass blending before the resolve (decals, particles, water, first-person
   transparents, additive light passes) blending compressed values.
7. **Judged by eye** and accepted (owner, 2026-11-23); no GPU timing.

`tools.exposure` ([tools.md](tools.md#exposure)) dumps the state, histogram, meter and bloom.
On acceptance the A/B switches `renderer.exposure`, `renderer.encodeSrgb` and `renderer.bloom`
were deleted (bloom always on, owner 2026-11-23); `bloomStrength` and the `exposure*` limits stay.

### 14. GTAO + lightmap channel ownership

GTAO for small-scale occlusion. Lightmaps are kept, with one owner per channel: inside cascade
range the cascades own sun visibility and the baked sun term is only the fallback beyond the last
cascade (as now); sky visibility and baked point lights stay baked. Lightmaps are not re-baked at a
higher resolution unless the reference views show the baked sky term failing.

Found 2026-11-23 (Kashan Desert, sun moved to 4° by `lightManager.sunDirection`): the baked sun
term is visibility × N.L for the level's authored sun, so with any other sun the map past the
cascades keeps its noon light in the new sun's colour while the cascades' range and the far ground
take the real cosine: a dark disc round the camera the size of the cascades' reach, and a bright
map against a dim far ground. At the authored sun they roughly agree. Proposed (agent, not yet
planned by the owner): own the sun term at run time everywhere, in three layers: the cascades
near (as now); one static cascade over the map (terrain and static meshes, about 1 m texels at
4096²), cached and redrawn only when the sun moves; the terrain's self-shadow out to the far
ground's edge, marched through the heightmap and the bake toward the sun into one visibility
texture, also only when the sun moves. The lightmaps' sun channel goes for terrain and static
meshes; sky visibility and point lights stay. Gives long low-sun shadows; loses the baked soft
building shadows at range.

### 17. Far terrain

Planned by the owner, 2026-11-22: cheap ground past the map for air combat, so a jet at altitude
never looks down at the void. Where it stands: the level's surrounding ring (eight sectors around
the primary heightmap, 8-bit heights, each the primary's size; none on some levels) is lit like the
terrain (sun N.L at its vertex normals, the sky's SH, aerial perspective) but has no shadows and
coarse normals. Past the ring is nothing: the sky shows, and below the horizon the sky repeats its
horizon row (left open by milestone 11). Sea levels already run sea cells to the horizon. Visual
only: no collision or gameplay out there.

- **How far.** With the realistic haze (8 km visibility, 1200 m falloff), a straight ray from
  altitude h down to the ground keeps 5 % contrast up to a slant length of 3 / (σ0 · H/h · (1 −
  e^(−h/H))): about 9 km from 1 km up, 13 km from 2 km, 17 km from 3 km, 25 km from 5 km. A grid
  radius of 30 km covers jets; the curvature drop hides the rest.
- **What is there now** (task 1, 2026-11-22; views `ft-*` on Kashan Desert, Fallujah West, Muttrah
  City, Saaremaa). The rings are not land: on each level they are flat and one colour (Kashan grey,
  Fallujah slate blue), at most a coastline and the river's or roads' meshes running on into them.
  Past the ring: on Kashan the sky's repeated horizon row; on Fallujah West (a desert city) sea cells
  with waves to the horizon (its ring is flat at 0 m, under the river's 22.5 m water level; the
  test's heights were also misread until task 2); on Muttrah City sea cells and ring
  sectors shaded differently, the tiles' squares visible from 2 km. From 600 m up the playable map
  reads as a slab with a cliff at its edge. So the far ground replaces the ring's look too: the ring
  is an input (heights, coast), not something to extend.
- **One draw.** A static camera-centred grid of concentric rings, cells growing with distance,
  snapped to its own cell size so it does not swim. The vertex shader displaces it from one
  height-and-colour texture per level (vertex texture fetch) and lowers it for the Earth's curvature
  (d²/2R, 125 m at 40 km), which hides the outer edge under the haze. It skips the area the ring
  (or the primary map, on levels without one) covers, and its heights are pinned to the ring's edge
  there. Shaded like the ring, by the shared lighting model, with normals from the height texture
  per pixel.
- **Baked per level, not generated** (owner, 2026-11-23). Procedural ground (the known heights
  pulled out under fBm/ridged noise sized from the map's relief, land and water carried out from
  the map's edges, colour from a height-and-slope palette under stochastic tiling) was built and
  dropped after the first look: from altitude it read as invented (strips of land extruded from the
  map's edges, uniform blobs), and no tuning makes it a place. Every level gets a baked file
  instead; until it has one it keeps the retail ring.
- **Real terrain, per level.** Each level ships a baked file: elevation from Copernicus GLO-30/90, coloured by ESA WorldCover
  land classes, each class taking a colour from the level's own colour map (satellite colours would
  clash with the art). Levels are artistic takes on their places, so the bake blends to them over a
  few km. A fictional level is baked from a real place that fits its region (owner, 2026-11-23),
  chosen per level. The bake tool and the per-level files live outside this tree; the engine only
  reads the file if it is there, and without one draws the retail ring. Placement is fitted, not hand-set: the tool searches GLO-90 around the
  named place for the primary heightmap over position, rotation and scale; the fit gives the
  level's latitude, longitude, heading, scale, height offset (a level's 0 m is not sea level) and
  vertical ratio, and a poor fit means the name is borrowed and the level stays generated.
  Two variants, tried side by side (owner, 2026-11-23): unscaled (real heights, a step at the
  blend where real relief is 10-20x the map's) and scaled (heights about a pivot times the fitted
  ratio). One file holds the real heights in the level's datum with the ratio and pivot; a console
  variable picks the variant and the height texture is rebuilt on the CPU (ring near the map,
  blended to the bake), so the shader is unchanged.
- **Which levels are real places** (review 2026-11-23, PR 1.9.0.0, 81 playable levels, by name,
  not yet checked by the fit): about 33 real places of relief like the map's (Fallujah West,
  Khamisiyah, Kashan, Debrecen, Normandy levels, Saaremaa, the Falklands, ...); about 18 real
  places whose real relief far exceeds the map's (Mestia, Bamyan, Korengal, Kunar, Dovre, Adak,
  Muttrah, ...); about 30 fictional, set in a real region. Of the 26 levels with jets, 16 are real
  places and 10 fictional, so the generated ground carries a large share of air combat on its own.
  Heights: about 45 levels use the 0.0025 vertical scale, capped at 163.8 m, and many reach it.
- **Cheap additions.** Cloud shadows from milestone 12's field on the far ground (one texture read);
  a detail texture fading in for a low pass near the edge. Past ~50 km, if needed, a horizon band
  baked into the sky at load.

Tasks:
1. Measure (done, above). Every PR level has nine heightmaps, but not every ring is data:
   Saaremaa's eight sectors are one placeholder file (byte-identical, no ring colormaps; the
   engine gets no ring vertices), Op Barracuda's and Operation Bobcat's are all flat at 0, the
   Falklands' whole cluster is flat (8 km, 64 m cells, sea off: its land is presumably meshes).
   Many rings have flat sectors: Ascheberg, Grostok and Battle of Debrecen 7 of 8, Kafar Halab and
   Operation Soul Rebel 5, Fallujah West and Brecourt Assault 4, others 1-3.
   As built for task 2 (`Geom/FarGround.cpp`, contract 21; `renderer.farGround` 0 draws the retail
   ring): seven levels of 64 x 64 cells, 16 m to 1 km, out to about 32 km from the camera, one draw
   of about 43,000 triangles; each level snapped on the CPU to twice its cell, its outer cells
   morphed onto the next level's grid from 18 to 28 cells out (16 to 26 until 2026-11-23: see below), the levels overlapping by one coarse
   cell. Heights: the ring's vertices into a grid at the ring's cell over its square (769 nodes a
   side on the measured levels), pull-push filled; 3 km past the ring they reach the base height.
   Colour: every patch's colormap shrunk into one texture (512 texels a side at most), read at the
   map's nearest edge and blurred by the distance past it; patches without a colormap take the others'
   mean. Lit by the sun toward `RASM[17]` under the cloud shadow, the sky's SH and aerial perspective,
   normals from the heights per pixel; inside the map the pixels are clipped. The ring had never had
   sun: it read `SUNDIRECTION`, which the engine does not set on the terrain effect, hence its flat
   grey. The ring's heights were misread by the sea test (`measureHeights`: the slot's height stream
   taken for grid positions); fixed. A level without a ring (Saaremaa) gets sea all round when its
   map's lowest point is under the sea level. Left for task 3: the far ground is darker and greyer
   than the map at its edge (the map's detail layers are not in its colour), the smear past the edge
   shows streaks, Fallujah West's sea.
2. The grid, its texture slot and the curvature, drawn over the ring's area too, with a flat ground
   at the base height and the playable map's edge colour: the void gone on every level.
3. Known ground and the sea, as built (2026-11-23; `renderer.farGround`: 0 the retail ring, 1 the far
   ground on levels with a bake, the ring elsewhere (default: no level has one yet), 2 the far
   ground everywhere, a placeholder past known ground: the known heights pulled out, falling to the
   base height over 6 km). Known ground: the map, sampled from the heightmap cluster, and each ring
   sector that is data; a sector with under 1 m of relief, or whose height stream hashes equal to
   another's, is a placeholder, and ring nodes under the sea deeper than 5 m below the map's
   lowest point are filler (Fallujah West: 2 of 8 sectors placeholders, 512 filler nodes; Muttrah
   City 7 of 8 data). Heights in two R32F textures, fine over the ring's square at its cell (769
   a side), coarse 1024 a side out to the map's half side plus 34 km (about 68 m), blended over
   1.5 km inside the fine square's edge; the bake fills the coarse one. A sea level counts only
   within 10 m under the map's lowest point to its highest (-50, -10000, ... mean none); with the
   far ground drawing, sea cells past the map draw only where its heights are under it (Fallujah
   West: the river's mouth, 14 coarse nodes, no sea to the horizon). Colour: each patch's
   colormap times its detail layers' mean albedo (pow(2 x texel, 2.2) over the atlas cell,
   weighted by the patch's mean weights), which was missing from the far colour (darker and
   greyer than the map); not yet judged on a bake.
4. Shading: per-pixel normals and cloud shadows (task 2); the detail fade as built: the detail
   record weighing most along the map's edge, decoded as the terrain does, divided by its mean,
   fading out from 500 to 1500 m from the camera. Not yet judged.
5. The bake file and its reader, the placement fit, and one real-place bake as the test: the
   Falklands (jets; its playable area is the islands), else Kashan or Khamisiyah (jets, low
   relief); the two variants compared where they differ, on a mountain level with jets (Kunar
   Province). Placement as built (2026-11-23, tool outside this tree): an automatic search (masked NCC over
   position, heading and scale on GLO-90) found no real match for Kunar Province (55 km around
   Asadabad) or Muttrah City; levels are placed by hand in a window over the real ground with the
   in-game map overlaid, the fit only refining. Placed: Kunar Province, Kashan Desert, Muttrah City
   (all heading 0, scale 1). The least-squares height ratio means nothing where the shapes do not
   correlate (Kunar 0.19): the scaled variant takes the ratio of the reliefs; on sea levels the
   offset is pinned so the level's sea lands on 0 m.
   The bake file and its reader as built (2026-11-23): `<mod>/settings/eon64-farground/<level>.bin`
   (layout at `FarGround.cpp` `loadBake`), int16 heights in the level's datum on a 60 m grid out
   to 36 km past the map's half side (about 1270 nodes a side, 3.2 MB), the GLO-90 ground at the
   placement resampled at 45 m. Datum: on a coastal level the real sea lands on the level's sea,
   with a sea bed 3 to 40 m under it by the distance from the coast; inland, the real ground's
   median under the known ground lands on the level's. The scaled variant (`renderer.farGroundScaled
   1`; 0 real heights, the default) is about a pivot (the sea, or the known ground's median) times
   the ratio of the reliefs above it (98th percentiles): Muttrah City x0.58, Kashan x0.15, Kunar
   x0.17. Past known ground the pulled-out known heights reach the bake over 3 km. An inland bake
   has no water past known ground (Kunar Province's river level at 36 m would flood the real
   valleys). The load logs the bake's correlation with the map, as placed and mirrored, to check
   the placement's orientation (Muttrah City 0.75 as placed, about 0 mirrored; Kunar 0.48). Seen
   in game on Muttrah City (`ridges`, `ft-top`) and Kunar Province (`ft-1500`, the variant
   switched live). Where it meets the map (2026-11-23, contract 23): the mesh starts at 4 m cells
   (nine levels, about 76,000 triangles with the strip), and an edge height texture at the map's
   cell (the map plus 256 m) holds the map's *drawn* heights: the compiled patches stitch their
   last row to the ring (Kunar Province's east edge drawn 6 m under the heightmap there, 11 m at
   most), so the patches hand their vertices within 64 m of the edge over as they load. Past the
   edge the ground (the ring's heights) is brought to the edge's height, fading out over 160 m
   (Kunar: 3.3 m off on average, 21 m at most). A strip 30 m deep under the edge closes gaps
   where the two surfaces simplify differently at range; the far ground reaches a pixel into the
   map, sunk under it (clip() drops whole pixels under MSAA); the curvature drop starts 2 km past
   the map. Seen on Kunar Province `edge-low`, `edge-e`: no step or gap. The clouds in the haze: [11](#11-atmosphere-sky-ibl-water-reflections)'s per frame
   (contract 25), reworked by [18](#18-atmosphere-rework). Colour past the map
   (2026-11-23, bake version 3, contract 29): ESA WorldCover's land class per 30 m node after the
   heights (sea nodes as water). Each class has a reference albedo in the engine
   (`kLandClasses`), tinted by the map's colour under the bake (one gain per channel over the
   classes found there; snow untinted), leaning up to halfway toward the class's own mean under the
   map (full at 3000 texels: placements are borrowed, so a class's own mean is only half
   trusted). The classes go into a 2048 x 2048 sRGB DXT1 texture over the bake's square; the
   shader blends to it from the blurred edge colour over 1.5 km past the map. Seen on Kunar
   Province (`ft-1500`, `jet`, `top-12k`: the valley, forest and bare slopes where the class map
   has them), Kashan Desert (`ft-2000-y0`: bare, villages and fields), Muttrah City (`ridges`).
   Open: from above the map reads darker than the ground round it (its tree meshes; the far
   ground has none); within a class the colour is flat.
   Cracks and crawl (owner, 2026-11-23, Kashan Desert from a jet; built, contract 31). One-pixel
   sky-coloured cracks along level boundaries came and went as the camera moved: where a finer
   level's snap is a coarse cell ahead it meets the coarser level's hole edge to edge, and those
   coarse edge vertices, up to 17 of their cells from the camera, were already morphing (from 16),
   so they slid off the finer level's. The morph now runs 18 to 28 cells. And the ground visibly
   moved as levels snapped and morphed: every vertex read the full-resolution heights, so a
   level's coarse vertices picked single 60 m samples that changed as it snapped. The fine and
   coarse height textures now carry box-filtered mips, and a vertex reads them filtered to its
   distance / 14 (about its level's cell at the hole's edge, twice it once morphed); by distance,
   not level, so coincident vertices of two levels still agree. The edge heights stay full
   resolution (the map's drawn edge). The pixel shader's normals read at their step's mip.
   Seen on Kashan Desert `fg-corner`, `fg-corner2`.
6. Bakes for the other levels, fictional ones from a real place that fits their region.
7. Judged by eye from a jet on the task 1 levels; GPU and CPU cost (one draw) with the final GPU
   pass. Tasks 1–5 seen by the owner and fine for now (2026-11-23).

### 18. Atmosphere rework

Accepted (owner, 2026-11-23). One atmosphere (Bruneton 2017, Hillaire 2020: milestone 11's sky
model) whose aerosol comes from the weather, in place of milestone 11's ground-haze layer (a grey,
fixed-density haze lit by the clear sky's colour, every level at sea level). The haze, the sky,
the far ground and the clouds all go through it.

- *Aerosol from visibility.* Extinction at the level's ground at 550 nm = 3.912 / V minus the
  molecules' there, never under 0.002 /km; uniform up to the mixing layer's top, then falling at a
  1.2 km scale height. The type sets single-scattering albedo, Cornette-Shanks g and the Ångström
  exponent α (extinction ∝ λ^-α): continental (0.9, 0.75, 1.3), maritime (0.98, 0.8, 0.5), dust
  (0.96 / 0.91 / 0.80 per channel, 0.7, 0.2), smoke (0.85, 0.65, 1.8).
- *Altitude.* The level's ground above sea level: `renderer.weatherAltitude` (the ground at the
  terrain's mean height), else the bake's sea offset (`farbake` writes it into bake header
  version 2: real elevation = level height + offset; Kunar Province 935 m, Kashan Desert 1187 m,
  Muttrah City -27 m), else world y 0 at sea level. The planet's surface stays at sea level, so a
  high level's horizon dips.
- *The sun.* The sun above the atmosphere is fitted once against a fixed reference air (25 km,
  continental, 1.5 km, at the level's altitude), so a hazier sky does not brighten the scene. The
  surfaces' sun is the authored one times this air's transmittance toward the sun over the
  reference's: `RaSunFactor` (c199, every pass, w 1 once built) multiplies `Ra_SunIrradiance`;
  vertex-lit foliage multiplies it in its pixel shader, particles take `RaSunFactorVertex`. The sun
  disc is the surfaces' sun. At 5 km: dust 0.14 / 0.15 / 0.16 (neutral), smoke 0.28 / 0.15 / 0.05.
- *Tables.* Transmittance, multiple scattering (Hillaire's isotropic approximation), sky view
  and cloud light on the CPU, on a worker when the weather or the authored light changes (about
  0.3 s); the last tables stay bound meanwhile. The marches step finer near their start. The sky
  view is built at the camera's height in 200 m bands (a jet above the mixing layer sees clear sky
  over a haze layer); the sky cube, SH and cloud light at the ground's mean height, rebuilt after
  each weather build. `view.shot` waits out these rebuilds.
- *Aerial perspective* (contract 27). The atlas (2080 x 32 on s9) integrates the whole model per
  tile and slice in planet coordinates: Rayleigh and aerosol in-scatter with the sun's phase, the
  sun through the transmittance table and the cloud shadow map, multiple scattering from its
  table; the sky's share of its light is dimmed to the clouds' mean transmittance and their light
  added (contract 16's terms). 32 slices over the scenery distance (40 km) on a cubic curve (fog
  end 700 m falls in slice 8), transmittance per channel. The atlas stores in-scatter over the sky
  block's light toward the tile ray's elevation; `Ra_GetAerial` multiplies the pixel's own ray's
  back, so far ridges take the sky right behind them, and returns `Ra_Air` (rgb in-scatter and
  transmittance; `Ra_AerialMeanT` for an alpha). View-space callers take the horizon's.
- *Clouds* (contract 28). The near clouds take the atlas at their mean distance along their ray
  (`Ra_AerialSlices`, `Ra_AerialLight` in the shaders' `RaAerial.fxh`), with the panorama's closed
  form until the atlas's first build. The panorama's air is the same model in closed form:
  Rayleigh from the observer's height, the aerosol's mean density along the ray from its column,
  the in-scatter the clear sky's radiance scaled by (1 - Ta(d)) / (1 - Ta(∞)). The panorama's rgb
  is compressed by its luminance per unit opacity (no grey outline on filtered edges).
- *Weather per level* (`Weather.cpp`). At each level load (terrain load) the six weather
  variables (`weatherVisibility`, `weatherAerosol`, `weatherMixing`, `weatherAltitude`,
  `cloudPreset`, `cloudCoverage`) go back to their defaults (25 km, continental, 1.5 km, auto,
  milestone 12's preset and coverage), then `<mod>/settings/eon64-weather/<level>.txt` runs, outside
  this tree: a line is a variable without `renderer.` and a value, `#` comments; lines before any
  section apply to every layer, `[<mode>]` and `[<mode> <size>]` apply in file order where they
  match the loaded layer. Values go through the console, keeping the variables' ranges; other
  names are logged and skipped. Fog end stays the gameplay view distance and its cut; weather
  never changes it. 25 km and not 40 by default: in clearer air the view-distance cut stands out.
- `tools.atmosphere` dumps the weather, the file and lines applied, the aerosol, altitude, the
  surfaces' sun factor and every table.

Contract 26: `RaAtmosphere`'s aerosol rows and `RaSunFactor` (table in [Shader contract](#shader-contract)).

Deleted: the ground-haze layer and its borrowed light (`renderer.hazeVisibility`, `hazeFalloff`,
`RaAerialHaze`), the authored fog colour as the haze's albedo (retail's fog curve stays under
water, in first person and in thermals), contract 25's cloud haze, contract 20's
light-times-opacity in-scatter. GPU cost waits for the final GPU pass.

Open, for later: the sun's aureole is too broad (a one-lobe phase at g 0.75–0.8 keeps 60 % of its
peak at 10°; a sharp forward lobe would tighten it; Muttrah City `sunup`). Above the mixing layer
the aerosol falls at 1.2 km, so from a jet the ground 20 km off sits under an optical depth of
about 2 and the clouds aloft at 8 km visibility under about 1.5; a sharper top (an inversion)
would thin both. Not in it: shadowed haze (light shafts), humidity-driven cloud cover, rain, fog
banks.

### 19. Hex-tiled detail textures

Planned by the owner, 2026-11-23, after a lattice seen on Muttrah City's far hills: tiled textures
repeat on a square grid the eye picks out at range. Replaces the plain tiled lookup with hex-tiling
(Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022): the plane is cut into hexagons about 0.6
tiles across, each reading the texture shifted by its own hash; the three around a pixel blend with
weights sharpened (exponent 7) and steered by their luminance (0.6), so contrast holds and no grid
repeats. A tap under 0.4 % of the weight is not read, so most pixels read one or two taps, not
three (the gradients are passed in, so the branch is safe). Retail's low-detail modulation stays on
top as the large-scale variation. Chosen over histogram-preserving stochastic texturing (needs a
preprocessed texture and LUT per layer, at odds with the atlases) and over two-tap noise variation
(blends show on contrasty textures). `renderer.hexTiling` (1) switches it; 0 tiles as retail.
The lattice is the shaders' `RaHex.fxh`, shared by the terrain and static meshes; contract 30.

- **Which textures.** A shift breaks a pattern laid on a grid and a lookup in a strip atlas, and
  which textures those are is PR's content, so a list outside the tree says
  (`<mod>/settings/eon64-hextiling.txt`, `Geom/HexTilingList.h`, read at each level load):
  `[terrain grid]` names terrain layers kept on the square grid (Muttrah City's octagon slabs,
  tiles, cobbles, crop rows; every other layer is hex-tiled); `[static hex]` names the static mesh
  detail textures hex-tiled (every other stays on the grid). There a name alone is a whole sheet;
  with V ranges, only those strips of a strip atlas (most of PR's building textures: plaster,
  bricks, tiles and signs stacked in strips that tile along U), shifted along U only so a lookup
  never leaves its strip. `/N` marks a sheet or strip laid on a grid of N tiles (roof slabs,
  concrete panels): its shifts step by 1 / N tile, so the joints stay continuous and only which
  slab shows varies.
- **Static meshes.** `RaShaderSTM`'s detail map and detail normal map read the same taps; the
  luminance steering comes from the colour. The material's `RaHexDetail` is set when its textures
  bind and applied with the mode zeroed while `renderer.hexTiling` is 0. Bundled meshes have no
  detail layer (one diffuse map on unique uv), so they are not touched.

- **No turn.** The reference turns each hexagon too; on PR's rock and sand that points their
  streaks every way (Muttrah City `mountain`: hairy, blotchy), so hexagons are only shifted, top
  plane and sides alike. Strata stay level.
- **Cost.** Up to three reads per plane where one was; not timed (owner, 2026-11-23: not needed).

Tasks:
1. Terrain and far ground (built 2026-11-23): every detail layer's planes in `RaTerrain`, and the
   far ground's detail fade (which carried the lattice: Muttrah City `valley`, the hills behind
   the mountain, A/B with `renderer.hexTiling 0`).
2. Static meshes (built 2026-11-23; owner: rooftops) and the list: Muttrah City's city tiles on
   the terrain kept on the grid (owner, 2026-11-23); `RaShaderSTM`'s detail by whole sheet, by
   strip, and in grid steps. Muttrah City `roofs` (city_paving_de and mecity_01_de's panels on the
   roofs, its plaster on the walls), `market`.
3. Judged by eye: accepted on Muttrah City (owner, 2026-11-23); no GPU timing (owner).

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
   full-screen pass at the far plane, filling what nothing covers), the near clouds over the scene
   and the sky (milestone 12, [Clouds](#12-clouds)), the fog pass, decals, the
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
   luminance histogram and the auto-exposure run on the scene ([milestone 13](#13-hdr-bloom--auto-exposure)).

Depth is reversed throughout (near 1, far 0) except in shadow-map depth buffers.

When deleting a retail pass, check what state it leaves bound for the passes after it: the
LightManager shadow pass bound the scene depth and RT0, and deleting it broke the scene until
`dc97ef7b`.

## GPU pass timers

`BF2_GPU_TIMERS` ([dev.md](dev.md)) and the performance overlay ([ui.md](ui.md)) time the frame
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

## Sun shadows

`SunShadows.cpp`. The cascades replace retail's per-caster dynamic shadows, which are gone (owner).
Casters come from a **submission context**: each mesh type's normal submission
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
| 8 | Aerial perspective (render milestone 11, task 4): the atlas (contract 6) is bound to the reserved sampler s9 (linear, clamp) after it draws, and pixel register c223 is the engine's `RaAerial` (1 / the viewport's width and height, 1 / fog end, w 1 while the atlas holds the world view's haze; zero in the first-person passes and under water), published with `RASM`. Lit shaders fog through `Ra_GetAerial` (VPOS and the distance pick the atlas texel; w 0 or thermals: retail's fog curve). `RaAtmosPerspective` also gets `RaSkyViewTable`: full haze is the sky's radiance along each ray. `RaAtmosphere` rows 8–10: [8] unused (the steepening, removed at contract 9), [9].rgb the sun at the ground, [10].rgb the sky light |
| 9 | View distance (render milestone 11, task 5): `RaAerial.w` is 2 while the radial cut applies (the world view with the atlas). `Ra_ViewDistanceFade` (1 inside, 0 at fog end and beyond, over the last 15 % by distance from the camera) and `Ra_ViewDistanceClip` (that fade against a 4x4 Bayer threshold, `Ra_Bayer4`) are called by every opaque world effect's main pixel shader, which its depth prepass calls too; blended effects (particles, trails, decals) take the fade on alpha. (`RaAerialHaze` in c199 and the closed form past fog end: replaced by contracts 26 and 27.) Static meshes (`RaShaderSTM`, and trees through the leaf and trunk effects), overgrowth and roads are not cut: the engine culls static meshes by size on screen, overgrowth at its own range |
| 10 | Sky cube (render milestone 11, task 7): the sky cube on the reserved sampler s10 (`A16B16G16R16F` cube, 128 per face, 6 mips, trilinear, clamp): mip m is GGX-prefiltered for perceptual roughness m / 5 (alpha its square), so `texCUBElod(dir, roughness * 5)`; the model's clear sky above the horizon (no sun disc), the lit ground hazed toward the horizon below it; in scene units, the drawn sky's scale. The split-sum BRDF table on s11 (64x64 `A16B16G16R16F`, bilinear, clamp): u n.v, v perceptual roughness, r scale and g bias on F0 (Smith GGX, k = alpha / 2). `RaSkySH` in pixel registers c192–c198 (engine-owned): the sky's irradiance / pi (the authored sky colour's unit) at normal n per channel ch = dot([ch], (n, 1)) + dot([3 + ch], n.xyzz * n.yzzx) + [6][ch] * (n.x² − n.y²); [6].w 1 while live, all zero (and s10, s11 unbound) before the first fit. Particle effects get the same 7 float4s as the parameter `RaSkyShVertex` (`rendererBindParticleLighting`), for their vertex shaders. Its sky part is scaled so its upward value keeps the authored sky colour's luminance; the ground's bounce is not scaled |
| 11 | One light (render milestone 11, task 8): the atmosphere's sun at the ground (`RaAtmosphere[9]`, the sky, disc and haze) is the surfaces' sun, the authored colour^2.2 x 2^2.2; the sky is physical under it, unscaled (the sky gain is gone, `RaAtmosphere[4].w` is 1); `RaSkySH` and the sky cube are in those units, unscaled, so a shader uses them as they are next to `Ra_SunIrradiance` |
| 12 | Water reflections (render milestone 11, task 12): just before the water queue draws, above water, with this frame's prepass, the scene colour is copied into a single-sample texture of its size and format on the reserved sampler s12 (bilinear, clamp; since render milestone 16, task 3, a full mip chain of 2 x 2 boxes, trilinear), and pixel register c191 is `RaWaterSsr` (1, 1 / width, 1 / height, 0); zero otherwise and after the water. While water draws, s15 holds the opaque linear depth only (water's own goes to the multisampled target, resolved later) |
| 13 | Sky clouds' volumes (render milestone 12, task 2): effect `Shaders/RaClouds`, techniques `RaCloudBuildField`, `RaCloudBuildCoarse`, `RaCloudBuildLight`, drawn by the engine (`Clouds.cpp`) a viewport per atlas tile into its own targets; parameters by semantic `RaClouds` (`float4[8]`: field world origin, size; slab base, top; toward the sun, LWC at density 1; droplet diameter, mid layer's density, shape tiles), `RaCloudWeather`, `RaCloudShape`, `RaCloudDetail` (volume textures), `RaCloudField`, `RaCloudCoarse`, `RaCloudLight`. Layouts in the shaders' `RaCloudField.fxh` and below |
| 14 | Sky clouds' panorama (render milestone 12, task 3): technique `RaCloudPanorama` in `Shaders/RaClouds`, drawn by the engine a band of rows at a time into its own targets, with `RaClouds` [4].y a jitter seed and [5]–[7] the panorama's (observer, drift, high cloud, wind; layout in the effect), `RaAtmosphere` rows 0–10 (camera rows zero), `RaSkyViewTable`, `RaCloudLightTable`. `RaSky` gets `RaSkyClouds` (`float4`: x the newest panorama's weight, y 1 while live) and `RaCloudPanoramaOld`, `RaCloudPanoramaNew` (2048x1024 `A16B16G16R16F`, linear, u wrapping); layout and decoding in the shaders' `RaCloudPanorama.fxh` |
| 15 | Reachable clouds (render milestone 12, task 4): `RaClouds` grows to 20 float4s ([8] the observer's x, z, the reference height and the handoff distance; [9]–[18] the near passes'; layout in `RaClouds.fx`), and [5].z is the observer's (camera's) y. The panorama covers the whole sphere from the camera (`RaCloudPanorama.fxh`). Techniques `RaCloudNear` (two targets), `RaCloudNearResolve`, `RaCloudNearComposite` with `RaCloudNearCurrent`, `RaCloudNearDepth`, `RaCloudNearHistory`, and the scene's linear depth on s15 |
| 16 | Cloud shadows and the light under the clouds (render milestone 12, tasks 7–8): pixel registers c189–c190 `RaCloudShadow` (engine-owned, published with `RASM`, zero outside the main world pass) and the cloud shadow map on the reserved sampler s14 (1024² `A16B16G16R16F`, bilinear, clamp; r optical depth toward the sun, g, b the world heights the sun ray enters and leaves cloud); particles get `RaCloudShadow` and `RaCloudShadowMap` (point) as parameters. Layout in the shaders' `RaSunShadow.fxh`. `RaAtmosphere[8]`: w the clouds' mean sun transmittance S, rgb the clouds' light on the air under them ((0, 0, 0, 1) clear). Technique `RaCloudShadow` in `Shaders/RaClouds` (`RaClouds[8]` the map's corner, the reference height, its size); `RaClouds[4].w` the panorama target's height. `RaSkyClouds.z` how far the camera is under the slab |
| 17 | Water's waves (render milestone 16, task 2): effect `Shaders/RaWater`, techniques `RaWaterSpectrum`, `RaWaterFft`, `RaWaterSlopes`, drawn by the engine (`Water.cpp`) each frame before the depth prepass; parameters by semantic `RaWater` (`float4[4]`, layout in the effect) and `RaWaterInput`. While the water queue draws, the three cascades' slope maps are on the reserved samplers s6–s8 (256² `A16B16G16R16F`, full mips, 16x anisotropic, wrap; r, g the slopes along and across the wind, the normalised `WaterScroll.xy` ((1, 0) when zero), b, a their squares, the last cascade's plus the slope variance below its grid) over patches of 503, 87 and 17.3 m, and `RaWaterSsr.w` is 1; zero without them. `RaWater[3]`: xy the wind's direction, zw the variance added to b, a |
| 18 | Water's foam (render milestone 16, task 5): the `Shaders/RaWater` atlas is 1024 × 256 (a fourth block: rg Jxz of cascades 0 + i 1, ba of cascade 2), technique `RaWaterFoam` with parameter `RaWaterHistory` (semantic) and `RaWater[0].w` the fade, `[1].w` the choppiness, `[3].zw` the threshold and 1 / ramp; while the water queue draws with waves, the 87 m and 17.3 m cascades' foam maps on s4–s5 (256² `R16F`, mips, 16x anisotropic, wrap) and pixel register c188 `RaWaterFoam` (x 1 while bound, y the depth waves break at, z the waves' loop seconds), zero otherwise |
| 19 | Bloom and auto-exposure (render milestone 13): effect `Shaders/RaHdrPost`, techniques `RaBloomDownKaris`, `RaBloomDown`, `RaBloomUp` (blends ONE, ONE), `RaLumMeter`, `RaLumHistogramRows`, `RaLumHistogram`, `RaExposureAdapt`, drawn by the engine (`HdrPost.cpp`) before the grade; parameters by semantic `RaHdrPost` (`float4[4]`, layout in the effect), `RaHdrSource`, `RaHdrHistory`. The FSQuadDrawer effect (pass 0, the grade) gets `RaGrade` (`float4[2]`: auto-exposure on, the bloom's share, sRGB encode, frame mod 64; 1 / the bloom's size, manual exposure in stops, 1 / level count), `RaGradeExposure` (the 1 x 1 state: adapted EV, exposure, metered EV, written) and `RaGradeBloom`; all zero means 0 stops, no bloom, the 2.2 encode. Particle effects get `RaExposureParticle` (`float4`: x 1 auto, y manual stops) and `RaExposureState` (the 1 x 1 state, for a vertex sampler) |
| 20 | The haze's light per pixel: the aerial-perspective atlas is 1056 x 32, its 33rd block (x 1024 + column, row r) the haze's light (rgb, a 1) toward the azimuth where tile column x's rays meet the horizon, at sin(elevation) = (r / 31)^2 (rays below the horizon read row 0); `Ra_GetAerial` lights a pixel by its own ray's elevation, its in-scatter that light times one minus the transmittance. |
| 21 | The far ground (render milestone 17): technique `RaFarGround` (pass 0, depth LESSEQUAL and written, no blend) on the terrain effect, drawn in the surrounding ring's place; its vertex stream one `SHORT4` POSITION0 (grid x, z in cells, level). Parameters by semantic `RaFarGroundLevels` (`float4[8]`: the level's origin x, z, cell, 1 when it morphs), `RaFarGround` (`float4[4]`: camera, 1 / 2R; the height texture's uv scale and offset, the ring's half side, 1 / the fade; the map's half side, colour uv per m, colour texels per m, the base height; the morph's start and 1 / range in cells, the height texel), textures `RaFarGroundHeight` (R32F, read in the vertex and pixel shaders) and `RaFarGroundColor` (A8R8G8B8, sRGB, mips). Layout in the engine's `FarGround.cpp` |
| 22 | The far ground's heights in two textures and its detail fade (render milestone 17, tasks 3–4): `RaFarGround` grows to `float4[8]` ([1] the fine heights' uv scale and offset, the fine square's half side, 1 / the blend to the coarse; [2] the map's half side, colour uv per m, colour texels per m; [3].z the fine cell; [4].xy the coarse heights' uv scale and offset; [5] the detail fade's atlas cell; [6] its atlas (-1 none), uv per m, the distance it is gone at, 1 / its range; [7].rgb 1 / its decoded mean), texture `RaFarGroundHeightFar` (R32F, vertex and pixel shaders); `RaFarGroundColor` carries the detail layers' mean; the terrain's `RaTerrainAtlas0`–`2` are bound while it draws. Layout in the shaders' `TerrainShader_FarGround.fxh` |
| 23 | The far ground meets the map's edge (render milestone 17 task 5): `RaFarGroundLevels` grows to `float4[10]` (slot 9 the strip under the map's edge: origin 0, cell 1; its vertices carry x, z and in w their depth under the edge, which every vertex subtracts); `RaFarGround[3].w` the edge square's half side, `[4].zw` the edge heights' uv scale and offset; texture `RaFarGroundHeightEdge` (R32F at the map's cell over the map and 256 m past it, vertex and pixel shaders); the curvature drop starts 2 km past the map; vertices inside the map sink half their cell and the far ground's pixels reach max(1 m, 0.4 % of their distance) into it (clip() drops whole pixels under MSAA) |
| 24 | The far ground's depth (render milestone 17): technique `RaFarGroundDepth` on the terrain effect (pass 0, depth GREATER (reversed) and written), the lit pass's vertex shader and clip, linear view depth out; the engine draws it after the map's `RaDepthPrepass` with the same parameters as `RaFarGround`, so s15 holds the far ground for the near clouds, soft particles, the transparents' depth rebuild and the water's thickness. Optional: without it s15 is empty (infinitely far) there |
| 25 | The clouds in the ground haze (render milestone 12; removed at contract 28): `RaClouds[18].zw` in the panorama's and the near clouds' passes, the haze's density at the reference height (1/m) and 1 / its falloff (0, 0 before the first fit); the clouds' air (`Ra_PanoAirTau`, `Ra_PanoAirTauInfinite`) includes it. The sky pass no longer hazes |
| 26 | One atmosphere (render milestone 18): `RaAtmosphere` rows 1–2 the aerosol's scattering and extinction (1/km in the mixing layer), 2.w its g, 3.w the sky view's height above sea level (km, the camera's 200 m band), 7 the ground's and the mixing layer's top heights above sea level (km), 1 / the aerosol's scale height above it, world y 0's height above sea level (m); 9 the surfaces' sun, 9.w fog end; textures `RaTransmittanceTable`, `RaMultiScatterTable` on the atlas pass. `RaSunFactor` (c199, every pass): the surfaces' sun over the authored, rgb, w 1 once built; `Ra_SunIrradiance` multiplies it in, `Ra_SunDecode` does not (vertex shaders; particles get `RaSunFactorVertex`). Layout in the shaders' `RaAtmosphere.fx` |
| 27 | The aerial-perspective atlas from the model (render milestone 18): 2080 x 32, 65 blocks of 32 x 32 tiles; slice i at 40 km x ((i + 1) / 32)^3: blocks 0–31 the in-scatter over the sky block's light toward the tile ray's elevation, 32–63 the transmittance (rgb), 64 the sky block (contract 20's layout, the clear sky's radiance). `Ra_GetAerial` returns `Ra_Air` (in-scatter and transmittance, rgb), its in-scatter times the sky block at the pixel's own elevation. The atlas pass reads `RaCloudShadow` and `RaCloudShadowMap` as parameters |
| 28 | The clouds through the one atmosphere (render milestone 18): `RaCloudNear` reads the atlas on s9 and `RaAerial` (c223) as the lit passes do (the shaders' `RaAerial.fxh`); `RaClouds[18].zw` unused (contract 25 removed); the panorama's air from `RaAtmosphere` rows 0, 2 and 7; the panorama's rgb compressed by its luminance per unit opacity, rgb / (1 + Y / (1 − a)) (`Ra_CloudCompress`; the engine's read-back of the ground panorama decodes the same) |
| 29 | The far ground's colour past the map (render milestone 17 task 5): `RaFarGround` grows to `float4[9]` ([8] the far colour's uv scale and offset, 1 / the blend from the edge colour in m past the map, 1 when drawn); texture `RaFarGroundColorFar` (sRGB, mips) over the bake's square, the bake's land classes coloured from the map's; bound to the map's colour when the level has no classes |
| 30 | Hex-tiled detail (render milestone 19): `RaTerrainLayerAtlas[i].w` 1 hex-tiles the layer's planes; `RaFarGroundC[2].w` 1 hex-tiles the far ground's detail fade; both 1 while `renderer.hexTiling` is on and the layer's texture is not in the list's `[terrain grid]`. `RaShaderSTM`'s `RaHexDetail` (float4[4], a template parameter of the detail literals): [0].x 1 the whole detail sheet, 2 only the strips whose frac(V) lies in [1].xy, [1].zw, [2].xy, [2].zw (shifted along U only), 0 none (and while `renderer.hexTiling` is 0); shift steps a tile, 0 any shift, [0].y for the sheet, [3] per strip |
| 31 | The far ground's filtered heights (render milestone 17): `RaFarGround` grows to `float4[10]` ([9] 1 / the fine heights' texel, 1 / the coarse heights' texel, the vertices' height spacing per m of distance); `RaFarGroundHeight` and `RaFarGroundHeightFar` carry full box-filtered mip chains, read by `tex2Dlod` at log2(spacing / texel); the edge heights stay one level; the morph runs 18 to 28 cells |

Samplers 9–15 are reserved for renderer frame resources (and 4–8 in water effects, contracts 17–18), and pixel registers
c188–c223 for `RaWaterFoam`, `RaCloudShadow`, `RaWaterSsr`, `RaSkySH`, `RaSunFactor`, `RASM`, `RaLodDither` and `RaAerial`. No effect may use them for anything else.
Every other sampler states `SRGBTexture` in its `sampler_state`: TRUE where it reads colour (the
hardware decodes before filtering), FALSE for data and outside the scene. D3D9 sampler state carries
over between effects, so neither value is left to default. The engine binds s9–s15 (and s6–s8 for water) with it FALSE.

Every `sampler_state` also states its filter. A material sampler (a colour, normal or detail texture
mapped onto world geometry) is 16x anisotropic: min `ANISOTROPIC`, mag and mip `LINEAR`. A data
sampler (lightmaps, light and blend maps, cube and lookup maps, render targets, UI) states point or
linear. The engine defines no filter: retail's caps- and setting-dependent `FILTER_*` defines and
`IS_NV4X` are gone, so the texture filter quality setting changes nothing and the effect cache
directory no longer carries it (`_3153_0`).

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s4–s5 | Water's foam maps, the two finer wave cascades (contract 18) | `R16F`, mipmapped | During the water queue only, while `RaWaterFoam.x` > 0 |
| s6–s8 | Water's slope maps, one per wave cascade (contract 17) | `A16B16G16R16F`, mipmapped | During the water queue only, while `RaWaterSsr.w` > 0 |
| s9 | Aerial-perspective atlas (2080x32: 32 in-scatter and 32 transmittance slices of 32x32 screen tiles, then the sky block, contract 27) | `A16B16G16R16F` | After the atlas pass (start of the depth prepass), rest of the frame; read only while `RaAerial.w` > 0 |
| s12 | Scene colour copy for water's reflections (contract 12) | The scene's (`A16B16G16R16F`), mipmapped | During the water queue only, while `RaWaterSsr.x` > 0 |
| s13 | Sun cascade atlas | `R32F` | From the cascade pass, rest of the frame |
| s14 | Cloud shadow map (contract 16) | `A16B16G16R16F` | From the clouds' frame step (before the prepass), rest of the frame; read while `RaCloudShadow[0].w` > 0 |
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
  ([Frame](#frame), item 3), then the far ground `RaFarGroundDepth` (contract 24).
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
retail shared one cache across mods). An entry is keyed (`.cfx.key`) by an FNV hash of the source,
every file reachable through its `#include` lines, the defines, the flags and the compiler, so
editing an included header invalidates every effect that includes it. `effects.lst` lists the
effects a level asked for; at a level's first effect compile the stale ones compile on worker
threads ([perf-load.md](perf-load.md#state)). Effects compiled by `compileEffectFile` are cached the
same way under `cache/fxsrc/`. Both compile with `d3dcompiler_47` ([milestone 15](#15-shader-pipeline)).

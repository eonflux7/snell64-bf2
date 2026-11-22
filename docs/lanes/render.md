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
  Moved to milestone 13: the compressed (Karis) resolve.

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
- *Ground haze:* density from fog end (`renderer.hazeDepth`, optical depth 1 there), albedo from the
  authored fog colour, density falling with height (`renderer.hazeFalloff`, 400 m). Exponential in
  optical depth throughout, past fog end too; retail's fog start is not used.
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
after the sun cascades) on s9; every fogged shader reads it per pixel (`Ra_GetAerial`). Retail's fog
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
- Full haze is the sky's radiance along the ray, so an aircraft against the sky and a vehicle
  against terrain vanish at the same distance.

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
- View distance: the far ground ring (land past the surrounding ring shows sky; below the horizon
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
  cloud panorama (about 1024x512; the 192x108 sky-view table is too coarse for cloud edges),
  updated over several frames, drifting with the wind. The marches are real loops: unrolled, the
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

**Reference set.** Poly Haven's outdoor HDRIs (CC0; 647, 454 with an open sky: 225 partly cloudy,
173 clear, 47 overcast, 7 fog; 285 from southern Africa, 225 from Europe, 120 without coordinates),
fetched at 2k–4k by a script and not stored in the repo; about 8 hand-picked ones, chosen just
before the prototype is judged. The sun's position comes from each one's
coordinates and date (or its brightest pixel), and the script checks its sun peak is unclipped. No
skies from the Middle East: the dry southern African ones stand in for the desert preset. Uses, in
order of value:
1. Side-by-side panoramas: our cloud panorama (same equirectangular layout) rendered at a reference's
   sun position, preset and coverage.
2. Luminance ratios within one image (sunlit cumulus against the sky beside it, cloud base against
   the horizon, overcast against clear zenith), measured in the reference and in ours. Where no
   cloud covers the sun, the sun disc against our atmosphere's scales the reference to absolute
   luminance.
3. The coverage-driven lighting, checked before the clouds exist: the scene lit from a real
   overcast reference's SH against what our coverage value gives.
4. Cloud statistics (coverage, size distribution) from a red/blue segmentation, masking about 5°
   around the sun for flare; only if the prototype's clouds look wrong (Dagor's noise is tuned).

The prototype (the two presets, the coverage value, the Mie phase and multiple scattering, before
the lighting is coupled) is judged on Kashan Desert from the ground, the air and inside a cloud at
coverage 0.3, 0.7 and 1.0, against the reference set by uses 1, 2 and 4. Its GPU time at
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
  Presets (`renderer.cloudPreset`): *desert* slab 3.5–9.5 km above the reference height (hot,
  dry: base 125 m × (T − Td) ≈ 3.5 km), cloud type 0.25–0.55, LWC 0.6 g/m³, droplets 12 µm
  (continental); *maritime* 0.5–5.5 km, type 0–0.3 (stratus to stratocumulus), 0.35 g/m³,
  20 µm. `renderer.cloudCoverage` (default 0.4) feeds Dagor's coverage curve; the mid layer
  takes 0.6 of it. Kashan Desert, desert preset, from above: the share of the sky with column
  optical depth over 1 is 0.14, 0.72 and 0.95 at coverage 0.3, 0.7 and 1.0 (seen from the
  ground towers' sides add to it); the clouds sit 3.6–7 km. Open, for the panorama (task 3) to
  show: the field's shapes read as fuzz from above rather than distinct cumulus cells, and at
  low coverage they bunch in one ~20 km weather region (Dagor's weather frequency, 2 per world).
  `tools.clouds` dumps every volume.
- *The panorama* (`Clouds.cpp`, `RaClouds.fx` technique `RaCloudPanorama`, `RaCloudPanorama.fxh`,
  contract 14), built while clouds stayed at infinity; task 4 makes it the far field seen from the
  camera's height. 1024x512 `A16B16G16R16F` over the upper hemisphere seen from the level's origin at
  the reference height (no parallax): u the azimuth, v sqrt(elevation / 90°), finer toward the
  horizon. rgb the light the clouds and the air in front of them add, stored as
  rgb / (1 + luminance); a the clouds' transmittance. Drawn a band of 64 rows a frame into a ring
  of three, each from the volume set and time latched at its first band (a build waits rather
  than overwrite the set a panorama reads); the sky (`RaSky`) blends the previous and the newest
  by the next one's progress, so a panorama fades in over 8 frames, in the compressed space, and
  multiplies the sky and the sun disc by a before adding rgb.
  - March: on spheres of the planet's radius, from the slab's base to its top or 150 km; steps of
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
    sky). Not measured: its GPU cost (at acceptance).

### 13. HDR bloom + auto-exposure

An energy-conserving bloom without threshold, and exposure that adapts between interiors and open
terrain, both before the frame grade. The tonemap's output dithered before its 8-bit encode: the sky's dark blue
bands without it (milestone 11). Bloom spreads the sun disc (luminance capped at 100) into its glare. Night layers need it: with the fixed exposure (then −1.25 stops; 0 since 2026-11-21)
The Falklands night terrain reaches the tonemap at about 0.0155, and Khronos Neutral's toe
(6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The adaptation needs an
upper limit so night stays dark (about retail's level), and the toe may need softening for what
stays below 0.08. Night particles (milestone 7) are checked here, on The Falklands night layer
(`gpm_cq 32`), once night exposure is settled.

The compressed (Karis) resolve, moved here from milestone 10 (owner, 2026-11-20), decided by an A/B
once auto-exposure runs: at a window seen from a dark interior and at foliage against the sky. It
pays only where an edge's samples straddle the tonemap's shoulder, which at the fixed exposure
(0 stops since 2026-11-21) is little beyond the sky and specular glints (milestone
11 handles those). Cost found in a first attempt: every scene pixel shader encodes through a flag
in an engine-owned pixel register, the resolve expands in the depth rebuild's full-screen pass, and
every pass blending before the resolve (decals, particles, water, first-person transparents,
additive light passes) blends compressed values, with additive sums clamped at the expansion.

### 14. GTAO + lightmap channel ownership

GTAO for small-scale occlusion. Lightmaps are kept, with one owner per channel: inside cascade
range the cascades own sun visibility and the baked sun term is only the fallback beyond the last
cascade (as now); sky visibility and baked point lights stay baked. Lightmaps are not re-baked at a
higher resolution unless the reference views show the baked sky term failing.

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

When deleting a retail pass, check what state it leaves bound for the passes after it: the
LightManager shadow pass bound the scene depth and RT0, and deleting it broke the scene until
`dc97ef7b`.

## GPU pass timers

`BF2_GPU_TIMERS` ([dev.md](dev.md)) and the performance overlay ([ui.md](ui.md)) time the frame
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
| 9 | View distance (render milestone 11, task 5): `RaAerial.w` is 2 while the radial cut applies (the world view with the atlas). `Ra_ViewDistanceFade` (1 inside, 0 at fog end and beyond, over the last 15 % by distance from the camera) and `Ra_ViewDistanceClip` (that fade against a 4x4 Bayer threshold, `Ra_Bayer4`) are called by every opaque world effect's main pixel shader, which its depth prepass calls too; blended effects (particles, trails, decals) take the fade on alpha. The atlas is no longer steepened: the haze stays exponential up to fog end. `RaAerialHaze` (c199, engine-owned, published with `RaAerial`): the haze's density at the camera's height and 1 / its height falloff; past fog end `Ra_GetAerial` integrates the haze in closed form along the pixel's ray (world-space positions). Static meshes (`RaShaderSTM`, and trees through the leaf and trunk effects), overgrowth and roads are not cut: the engine culls static meshes by size on screen, overgrowth at its own range |
| 10 | Sky cube (render milestone 11, task 7): the sky cube on the reserved sampler s10 (`A16B16G16R16F` cube, 128 per face, 6 mips, trilinear, clamp): mip m is GGX-prefiltered for perceptual roughness m / 5 (alpha its square), so `texCUBElod(dir, roughness * 5)`; the model's clear sky above the horizon (no sun disc), the lit ground hazed toward the horizon below it; in scene units, the drawn sky's scale. The split-sum BRDF table on s11 (64x64 `A16B16G16R16F`, bilinear, clamp): u n.v, v perceptual roughness, r scale and g bias on F0 (Smith GGX, k = alpha / 2). `RaSkySH` in pixel registers c192–c198 (engine-owned): the sky's irradiance / pi (the authored sky colour's unit) at normal n per channel ch = dot([ch], (n, 1)) + dot([3 + ch], n.xyzz * n.yzzx) + [6][ch] * (n.x² − n.y²); [6].w 1 while live, all zero (and s10, s11 unbound) before the first fit. Particle effects get the same 7 float4s as the parameter `RaSkyShVertex` (`rendererBindParticleLighting`), for their vertex shaders. Its sky part is scaled so its upward value keeps the authored sky colour's luminance; the ground's bounce is not scaled |
| 11 | One light (render milestone 11, task 8): the atmosphere's sun at the ground (`RaAtmosphere[9]`, the sky, disc and haze) is the surfaces' sun, the authored colour^2.2 x 2^2.2; the sky is physical under it, unscaled (the sky gain is gone, `RaAtmosphere[4].w` is 1); `RaSkySH` and the sky cube are in those units, unscaled, so a shader uses them as they are next to `Ra_SunIrradiance` |
| 12 | Water reflections (render milestone 11, task 12): just before the water queue draws, above water, with this frame's prepass, the scene colour is copied into a single-sample texture of its size and format on the reserved sampler s12 (bilinear, clamp), and pixel register c191 is `RaWaterSsr` (1, 1 / width, 1 / height, 0); zero otherwise and after the water. While water draws, s15 holds the opaque linear depth only (water's own goes to the multisampled target, resolved later) |
| 13 | Sky clouds' volumes (render milestone 12, task 2): effect `Shaders/RaClouds`, techniques `RaCloudBuildField`, `RaCloudBuildCoarse`, `RaCloudBuildLight`, drawn by the engine (`Clouds.cpp`) a viewport per atlas tile into its own targets; parameters by semantic `RaClouds` (`float4[8]`: field world origin, size; slab base, top; toward the sun, LWC at density 1; droplet diameter, mid layer's density, shape tiles), `RaCloudWeather`, `RaCloudShape`, `RaCloudDetail` (volume textures), `RaCloudField`, `RaCloudCoarse`, `RaCloudLight`. Layouts in the shaders' `RaCloudField.fxh` and below |
| 14 | Sky clouds' panorama (render milestone 12, task 3): technique `RaCloudPanorama` in `Shaders/RaClouds`, drawn by the engine a band of rows at a time into its own targets, with `RaClouds` [4].y a jitter seed and [5]–[7] the panorama's (observer, drift, high cloud, wind; layout in the effect), `RaAtmosphere` rows 0–10 (camera rows zero), `RaSkyViewTable`, `RaCloudLightTable`. `RaSky` gets `RaSkyClouds` (`float4`: x the newest panorama's weight, y 1 while live) and `RaCloudPanoramaOld`, `RaCloudPanoramaNew` (1024x512 `A16B16G16R16F`, linear, u wrapping); layout and decoding in the shaders' `RaCloudPanorama.fxh` |

Samplers 9–13 and 15 are reserved for renderer frame resources, and pixel registers
c191–c223 for `RaWaterSsr`, `RaSkySH`, `RaAerialHaze`, `RASM`, `RaLodDither` and `RaAerial`. No effect may use them for anything else.
Every other sampler states `SRGBTexture` in its `sampler_state`: TRUE where it reads colour (the
hardware decodes before filtering), FALSE for data and outside the scene. D3D9 sampler state carries
over between effects, so neither value is left to default. The engine binds s9–s13 and s15 with it FALSE.

Every `sampler_state` also states its filter. A material sampler (a colour, normal or detail texture
mapped onto world geometry) is 16x anisotropic: min `ANISOTROPIC`, mag and mip `LINEAR`. A data
sampler (lightmaps, light and blend maps, cube and lookup maps, render targets, UI) states point or
linear. The engine defines no filter: retail's caps- and setting-dependent `FILTER_*` defines and
`IS_NV4X` are gone, so the texture filter quality setting changes nothing and the effect cache
directory no longer carries it (`_3153_0`).

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s9 | Aerial-perspective atlas (1024x32: 32 distance slices of 32x32 screen tiles) | `A16B16G16R16F` | After the atlas pass (start of the depth prepass), rest of the frame; read only while `RaAerial.w` > 0 |
| s12 | Scene colour copy for water's reflections (contract 12) | The scene's (`A16B16G16R16F`) | During the water queue only, while `RaWaterSsr.x` > 0 |
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
retail shared one cache across mods). An entry is keyed (`.cfx.key`) by an FNV hash of the source,
every file reachable through its `#include` lines, the defines, the flags and the compiler, so
editing an included header invalidates every effect that includes it. `effects.lst` lists the
effects a level asked for; at a level's first effect compile the stale ones compile on worker
threads ([perf-load.md](perf-load.md#state)). Effects compiled by `compileEffectFile` are cached the
same way under `cache/fxsrc/`. Both compile with `d3dcompiler_47` ([milestone 15](#15-shader-pipeline)).

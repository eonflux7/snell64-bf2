# Renderer

The `render` lane replaces BF2's forward, gamma-space, lightmap-lit renderer with a linear HDR
pipeline built on a depth prepass and cascaded sun shadows, and later on a shared atmosphere,
image-based ambient specular and screen-space ambient occlusion. It is a new design: retail
behaviour is not a constraint, and an accepted milestone leaves no legacy path behind it.

The engine side is mostly in `Code/BF2/RendDX9/` (`Frame.cpp`, `BF2Render.cpp`,
`SunShadows.cpp`, `StateManager.cpp`, `RenderQueues.cpp`, `Ra/EffectManager.cpp`). The shaders are
PR's, kept in a private repository; these documents describe only the contract between the two.

This document is the plan: the roadmap and the milestones still to do. What the renderer does today
is in [renderer-pipeline.md](renderer-pipeline.md).

## Roadmap

| Milestone | State | Engine commits |
|---|---|---|
| 1. Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| 2. Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| 3. Shared lighting model | Accepted (shader-only) | — |
| 4. Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| 5. Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| 6. Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| 7. Particle lighting | Accepted | `b1549478`, `25319adf`, `7fcdc848`, `423ac5e0`, `0bffa203`; fix `6eb1a727` |
| 8. Anti-aliasing | Accepted | `28487a35`, `65760125`, `f11bdfe3`, `b47ac87f`, `0f10eb41`, `f9086318`, `ee0e7ed8`, `98165f0b`, `b3e44a8d`, `a7d9e0e1`, `1e7a1c89`, `680d8890` |
| 9. Draw submission | Accepted | `2eb902ce`, `0a623df8`, `e87543df`, `f3515fb4`, `7bdd3a77`, `4bc56ff8`, `e9b6a8a4`, `965bb8bf`, `c2f7ef33`, `3c745649`, `2002788d` |
| 10. Terrain in one pass | Accepted | `2eb4d02d`, `c21a012e`, `0d9e15a6`, `57fa2899`, `3b9da48c`, `6fd523bb`, and the deletion ("milestone 10 accepted") |
| 11. Atmosphere, sky IBL, water reflections | In progress | — |
| 12. Clouds | Not started | — |
| 13. HDR bloom + auto-exposure | Not started | — |
| 14. GTAO + lightmap channel ownership | Not started | — |

The owner accepts each milestone in game before the next starts. From milestone 9 on that is done
against the `tools` lane's reference views and benchmark paths ([tools.md](tools.md)), with the GPU
pass timers ([renderer-pipeline.md](renderer-pipeline.md#gpu-pass-timers)) and the debug overlay
([ui.md](ui.md)). Each milestone from 9 on also states its frame-time change on the benchmark paths
against the previous milestone's baseline; a regression stands only if the owner accepts it
explicitly, and is recorded with the milestone in [renderer-pipeline.md](renderer-pipeline.md).

Order: draw submission comes before terrain and the atmosphere, so the new terrain draw is written
once on the new path and the parameters milestones 11–14 add cost nothing per draw. Terrain comes
before the atmosphere, so aerial perspective and sky reflections are written once into a single
terrain pass. Bloom and auto-exposure depend only on the HDR scene and can move earlier if night
layers need them sooner.

Not planned: converting the assets to a PBR material set. The existing maps are interpreted as PBR
inputs (milestone 11) instead.

### 10. Terrain in one pass

Accepted by the owner, 2026-11-20; as built in [renderer-pipeline.md](renderer-pipeline.md#terrain).
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

The goal is a visibly more realistic outdoor image. Sky, sun and haze coming from one model is the
means, not the goal. The owner judges every A/B by eye in game, against milestone 10 at the
reference views. What should change on screen, and so what to look at:

- **Distance.** Far terrain and buildings lose contrast and take on the sky's colour with distance,
  so ridges separate into layers. Valleys hold more haze than ridgelines; the haze is brighter and
  warmer toward the sun, bluer away from it. Retail fog is one colour by distance.
- **Sky.** The sky comes from the model (clear until milestone 12's clouds), and its horizon meets the haze over the terrain without a
  seam. The sun has a disc and a forward-scattering glow from the same model.
- **Sky light.** Surfaces are lit by the sky from its direction: blue from above, darker and
  ground-tinted from below, brighter toward the sun, instead of one flat colour.
- **Materials.** Glossy surfaces reflect the sky, most at grazing angles, each material class in its
  own way: paint and glass sharp, metal tinted, fabric and concrete matte. A surface's sun highlight
  and its sky reflection agree, since both come from one roughness and F0. Glossy edges do not
  sparkle in motion.
- **Light shafts.** Sunlight in the haze is shadowed by the cascades, so shafts show between
  buildings and through trees.
- **Water.** It reflects the buildings, terrain and sky around it, and the sky where a reflected ray
  leaves the screen.

Design:

- **Two layers.** A sky atmosphere (Rayleigh and Mie, Earth's scale heights) drives the sky, the
  sun disc and the sky cube. A ground haze layer (density falling exponentially with height) drives
  aerial perspective. One atmosphere dense enough to reach full haze at PR's fog end (0.5–2 km)
  would turn the whole sky into haze, so they cannot be one.
- **Fit at load.** (1) Sunlight above the atmosphere = authored sun / transmittance at the sun's
  elevation, the transmittance clamped to at least 0.1 per channel (low suns, night layers),
  evaluated with the sky's own parameters at every step of (2). (2) The sky: Earth's air on every
  level (Rayleigh, Mie and ozone at Earth's densities and colour), a clear sky. One **sky gain** per
  level scales the scattered light so the azimuth-averaged horizon at 1° meets the authored fog
  colour's luminance, applied as gain^`renderer.skyGainBlend` (0 a physical sky, 1 the authored
  level; default 0.5, settled at task 3's sky A/B). The sun (surfaces, disc) is never scaled.
  (3) The haze layer: density from fog end, albedo from the authored fog colour, one height
  falloff for all levels, tuned at the aerial-perspective A/B.
- **Haze curve.** Exponential in optical depth throughout, continuing past fog end for the scenery
  there; objects end at fog end by the radial dither, not by full haze (owner, 2026-11-20, at task 5:
  the steepening to full haze at fog end would also hide the land drawn past the limit; task 4 had
  steepened it). Retail's fog start is not used: the exponential curve keeps the near range clear.
- **Tables.** Transmittance (256x64), multiple scattering (32x32) and sky view (192x108, taken at
  the terrain's mean height) built on the CPU with the fit (the sun does not move), by the same
  code the fit evaluates, into managed textures that survive device resets. Per frame only the
  aerial-perspective atlas, a pixel pass, with cascade shadowing per slice for the shafts.
- **Sky cube.** Upper half: the model's sky (and milestone 12's clouds once they exist). Lower half: the ground, the terrain
  colormap's mean colour lit by the authored sun and sky, under the haze at the horizon (sky alone
  below the horizon makes every glossy surface look chrome). Prefiltered GGX, one mip per
  roughness; its irradiance as 9 spherical-harmonic coefficients, taken at load.
- **Sky light.** Every receiver's sky term is the irradiance at its normal times its occlusion:
  statics and terrain the lightmap's sky visibility, vehicles and soldiers the hemi map,
  first-person models the sky visibility under the camera. The irradiance is scaled so its upward
  value keeps the luminance of the authored sky colour: each level keeps its balance, and only the
  direction and colour change.
- **Lighting model.** Direct sun specular and ambient specular from the same roughness and F0 (GGX,
  split sum). Material interpretation without new assets: gloss maps become roughness, and each
  class (paint, glass, metal, fabric) has its own F0 and roughness range. The engine holds the class
  index and the table's format; the mapping from PR materials to classes lives with the shaders,
  outside this tree. Vehicle environment maps stop being blended into albedo and become specular.
- **Specular anti-aliasing.** Roughness widens with the screen-space rate of change of the normal,
  and normal-map mips shorten the normal, which is read back as extra roughness (Toksvig). MSAA
  shades once per pixel, so without this glossy surfaces sparkle.
- **Water.** Screen-space reflections. The scene is resolved to a texture just before water draws,
  after the sky, the fog pass, decals and the particles drawn before water (also the source for
  refraction); water marches against the linear depth and falls back to the prefiltered sky where
  the ray leaves the screen. No reflections from under the surface: seen from underwater, water
  stays as it is.
- **View distance** (owner, 2026-11-20). Each level's view distance (fog end) stays the gameplay
  limit, so no level is retuned; what changes is how the limit looks and that it is the same in
  every direction.
  - **Radial distance.** Geometry is cut off at the distance from the camera, not the flat far
    plane, as the haze already is (its distance is radial). Today the screen's edges and corners
    reach up to 30–40 % further than its centre, an advantage for whoever turns a target to the edge.
  - **Fade to the background.** Objects fade toward the radiance behind them along the ray (the sky,
    or the hazed terrain), not toward one fog colour, so an aircraft against the sky and a vehicle
    against terrain vanish at the same distance (retail fades to one fog colour, so the aircraft's
    silhouette survives against a sky of another colour; task 4's full haze is already the sky's
    radiance along the ray, to be checked from jets and anti-air). The view distance is the distance at which an object's
    contrast reaches zero.
  - **Soft edge.** The last 10–15 % fades by alpha-to-coverage dither (4x MSAA), no popping.
  - **Scenery past the limit.** The level's low-detail surround terrain drawn out past the view
    distance, then a far ground ring (or sea on coastal levels) in the surround's mean colour to the
    horizon, all under aerial perspective; no vehicles, objects or vegetation out there. Aircraft
    see a landscape to orient by instead of a fogged void. Gameplay effect, accepted by the owner:
    the land's shape is visible further, targets are not.
- **Thermals.** Thermal vision keeps its own formula and takes none of this.
- **Budget.** Target for the whole milestone: at most 1 ms on the benchmark paths at 1920x1080
  (atlas 32x32 with 32 slices, sky view 192x108, cube 128 per face), measured at acceptance. Above
  it the regression rule applies.

Decisions (owner, 2026-11-20):
- The model draws the whole sky; the painted dome and its cloud layers are not shown (owner,
  2026-11-20, at task 3: a modern sky, not constrained by the game's approach; faithfulness to
  each level is not the goal). The sky is clear until volumetric clouds (milestone 12). The sky's air
  is Earth's on every level, physically correct; overcast comes from the clouds (owner,
  2026-11-20). Fitting the Rayleigh tint and density to the dome's zenith was tried and dropped:
  the domes' colours are mostly painted cloud and haze, so Kashan's clear sky read grey and
  overcast Saaremaa's navy. Earlier decisions kept the painted dome, first
  down to the horizon, then above 20°, then its cloud layers.
- The authored sun colour (`Lightmanager.staticSunColor`) stays the sunlight at the ground, and lit
  surfaces keep using it. The model works backwards from it to the sunlight above the atmosphere,
  so the sky, the sun disc and the haze agree with it.
- The authored fog end stays the visibility limit (PR's view distance; the far plane and culling
  follow it): aerial perspective reaches full haze there.
- The authored dome and fog are brighter than a physical sky under the authored sun (by day the
  horizon needs 2.7x on Muttrah City, 3.1x on Saaremaa, 5.1x on Fallujah West; 0.11x at night),
  and a full physical fit (Rayleigh and Mie densities free) ran to its bounds on Fallujah West. The
  sky keeps physics for its shape and colour and takes a per-level sky gain, partly applied, set by
  `renderer.skyGainBlend` (task 2).
- Bloom stays in milestone 13. The sun's glow and specular glints read fully only then, and are
  judged here knowing that. (Delegated, 2026-11-20.)

Survey (task 1, `tools.survey`, [tools.md](tools.md#survey)), 2026-11-20, at the spawn camera of
Muttrah City, Saaremaa, Fallujah West, The Falklands (`gpm_cq 32`, night) and test_bootcamp (for its
vehicles), and at Muttrah City's views `high` and `mountain`:
- **Sky dome.** One 2048-wide texture with no mips, a painted gradient from zenith (top row) to
  horizon (bottom row), plus one or two cloud layers; the technique drawn is the sun-flare one on all
  five. The bottom row sits a little darker and bluer than the authored fog colour (Muttrah City
  158 186 206 against fog 181 203 206; Saaremaa 172 184 188 against 176 187 189), so the dome
  already meets the fog roughly; its top row is the zenith input to the fit.
- **Fog.** End 490–1000 m by day, 2900 m on the night layer; start 0–1 m except at night (1000 m).
  Fog colours run from near white (Fallujah West 208 211 213) to near black at night (4 8 12).
- **Sun.** Elevation 23–43°; the night layer's "sun" (the moon) stands at 36° with colour 0.07.
  Static sun and sky colours differ a lot by level (Fallujah West's static sky 0.79 is brighter
  than its sun 0.48), which the irradiance scaling to the authored sky luminance keeps.
- **Terrain.** 512² DXT1 colormaps per patch; their mean colour is read at load in the survey's way
  (a level at most 16 wide). The Falklands' file heights read 0–0 over 4 patches: the ground term's
  height cannot come from the file there; take it from the patches or the camera.
- **Water.** All levels pick the high-end technique; water's env map is the level's
  `Water/envmap` or `Envmaps/EnvMap0` (the Falklands: a generated cube). Water covers 39 % of the
  screen at Saaremaa's spawn camera, 10 % on the Falklands; Muttrah City's views show almost none.
- **Retail environment maps.** Each level has 1–132 placed 128² DXT1 cubes (`Envmaps/*.dds`, read
  by `EnvironmentMapInfo.emi` at load; generated only in the editor); no env-map templates. Per
  frame the manager picks the cube nearest the camera, and only bundled-mesh materials whose state
  has the env flag sample it, through `CubeMap`; water binds its own cube. Both go when the sky cube
  replaces them (tasks 9 and 12).
- **Materials.** Bundled states sort by technique name and flags: env-mapped opaque and blended
  techniques (paint and glass candidates), colour-map gloss (gloss in the colour map's alpha),
  alpha test, cockpit. Static materials carry only combo bits (base, detail, dirt, crack, normal
  detail, parallax), no gloss or env source; `SpecularPower`, `StaticGloss` and `Reflectivity` are
  defaults on almost every material. The class table therefore keys on the technique name and
  flags.
- **Normal maps.** 193 of 200 sampled (DXT1 and DXT5) keep |n| ≥ 0.95 down to their smallest mip:
  the mips were renormalized, so Toksvig has no length to read. Specular anti-aliasing uses the
  screen-space normal derivatives alone.
- **Samplers.** The 21 legacy effects use s0–s8 (s8 by one) plus s13 and s15; the 611 Ra effects
  keep no bytecode (they are created not-cloneable), and their sampler parameters bound them to
  s0–s7. s9–s12 and s14 are free: the atlas, the sky cube and the split-sum BRDF table each get a
  reserved slot (no analytic fit needed).

D3D9 limits: there is no compute and no rendering into volume textures. The load-time tables are
built on the CPU (pixel passes were tried first: loops the game's D3DX compiler leaves as loops
came out wrong under DXVK, the multiple-scattering table 10x low); per-frame passes unroll their
loops. The aerial-perspective volume is a 2D atlas of depth slices, blended between
neighbouring slices by hand; particles read it per vertex, so its format must allow vertex texture
fetch (checked in task 2). Frame resources read by every lit shader get reserved samplers like s13
and s15: the atlas, the sky cube, and the split-sum BRDF table if task 1 finds a third free slot
(otherwise an analytic fit). The water scene texture and the sky's transmittance table are effect
parameters. Each task that adds to what the shaders may rely on raises the contract version (6 on).

### 12. Clouds

Decided by the owner, 2026-11-20: the levels' painted cloud layers are replaced by modern clouds,
not constrained by the game's approach; the sky is clear until then. Clouds must not change
gameplay: aircraft have no ceiling the clouds could sit above, so nothing that hides a target is
placed where aircraft fly. Three layers (owner, 2026-11-20); to be planned in full when milestone 11
is accepted:

- **Sky clouds (first).** Overcast decks, high cloud and distant banks drawn into the sky at
  infinity, so they are never reached, flown above or into, and hide nothing. Raymarched per
  direction through a cloud slab (Schneider, Horizon Zero Dawn / Nubis; Hillaire): Perlin-Worley
  shape and erosion noise built on the CPU at load (D3D9 samples volume textures but cannot render
  into them), a short march toward the sun, a dual-lobe phase, the powder term, a
  multiple-scattering approximation, the sky-view table as ambient, aerial perspective toward the
  horizon. Rendered into a cloud panorama of its own (about 1024x512; the 192x108 sky-view table is
  too coarse for cloud edges), updated over several frames, drifting with the wind. No parallax, so
  it carries no near cumulus. Loops unrolled (the compiler's loops go wrong under DXVK).
- **Coverage drives the lighting.** One coverage value per level (0 clear, 1 storm; picked per
  level outside this tree, a default otherwise) sets the clouds and the light together: the sun
  dimmed and its shadows softened, sky light greyer, more even and relatively brighter, the haze and
  reflections greyer through the sky they read. Overcast reads through the lighting, not the
  texture.
- **Sparse 3D clouds (later, its own task).** Small cumulus, and under overcast low ragged scud
  below the deck, in the flight space and flown through: clusters of lit soft particles (no
  raymarching, no temporal reprojection). Thin and sparse, so they hide almost nothing; they give
  parallax and the feel of flying under weather.
- Cloud shadows on the ground (a 2D map from the sun, scrolling with the wind) and into the sky
  cube.

First task: a prototype of the sky clouds and the coverage value alone, judged on Kashan Desert
from the ground and the air at coverage 0.3, 0.7 and 1.0, before the lighting is coupled. Budget
about 0.3–0.6 ms at 1920x1080, measured on the benchmark paths at acceptance.

### 13. HDR bloom + auto-exposure

An energy-conserving bloom without threshold, and exposure that adapts between interiors and open
terrain, both before the frame grade. The tonemap's output dithered before its 8-bit encode: the sky's dark blue
bands without it (milestone 11). Bloom spreads the sun disc (luminance capped at 100) into its glare. Night layers need it: with the fixed exposure (−1.25 stops)
The Falklands night terrain reaches the tonemap at about 0.0155, and Khronos Neutral's toe
(6.25·x² below 0.08) shows it at 5/255, where retail drew about 30/255. The adaptation needs an
upper limit so night stays dark (about retail's level), and the toe may need softening for what
stays below 0.08. Night particles (milestone 7) are checked here, on The Falklands night layer
(`gpm_cq 32`), once night exposure is settled.

The compressed (Karis) resolve, moved here from milestone 10 (owner, 2026-11-20), decided by an A/B
once auto-exposure runs: at a window seen from a dark interior and at foliage against the sky. It
pays only where an edge's samples straddle the tonemap's shoulder, which at today's fixed exposure
(−1.25 stops, Neutral linear to about 0.8) is little beyond the sky and specular glints (milestone
11 handles those). Cost found in a first attempt: every scene pixel shader encodes through a flag
in an engine-owned pixel register, the resolve expands in the depth rebuild's full-screen pass, and
every pass blending before the resolve (decals, particles, water, first-person transparents,
additive light passes) blends compressed values, with additive sums clamped at the expansion.

### 14. GTAO + lightmap channel ownership

GTAO for small-scale occlusion. Lightmaps are kept, with one owner per channel: inside cascade
range the cascades own sun visibility and the baked sun term is only the fallback beyond the last
cascade (as now); sky visibility and baked point lights stay baked. Lightmaps are not re-baked at a
higher resolution unless the reference views show the baked sky term failing.

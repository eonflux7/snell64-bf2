# Renderer: atmosphere, sky and water reflections

Part of [the renderer as built](README.md).

## 11. Atmosphere, sky IBL, water reflections

Accepted by the owner, 2026-11-22: a more realistic outdoor image, judged by eye at the reference
views of Muttrah City, Fallujah West and Saaremaa against milestone 10. Sky, sun, haze and sky
light come from one physical model under the level's authored sun; glossy surfaces and water
reflect it. No switch is left: the retail dome, fog curve (except where noted), flat sky light and
environment maps are gone from the lit path.

**One light, physical** (D-8). The sun at the ground is the one every lit surface
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
  from the authored fog colour, density falling with height (`renderer.hazeFalloff`, 1200 m; both variables deleted).
  Realistic, mostly in the distance (D-13); it was optical depth 1 at fog end (400 m
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

**View distance** (D-6). Fog end stays each level's gameplay limit; what changed is how
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
  but the painted dome's stars and night clouds are gone (D-12).
- Light shafts (dropped for now, owner 2026-11-21); thermals and under water keep their own looks.
- Point lights, baked and dynamic, not checked against the new sun.
- Decals' sky is not occluded (the hemi map holds only the ground's sky visibility); first-person
  models take the open sky rather than the visibility under the camera; weapons' metal and fabric
  under colour-map gloss have no class of their own.
- View distance: the far ground ring (milestone 17; land past the surrounding ring shows sky; below the horizon
  the sky repeats its horizon row), vegetation past overgrowth's range (milestone 22), jets and anti-air not
  checked, the far scenery's cost.
- Banding in the dark blue and the sun's glare: milestone 13 (dither, bloom).

## 18. Atmosphere rework

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

Contract 26: `RaAtmosphere`'s aerosol rows and `RaSunFactor` (table in [Shader contract](contract.md#shader-contract)).

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

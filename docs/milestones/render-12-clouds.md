---
id: render-12
title: Clouds
state: paused
updated: 2026-11-23
next: 10 (night layers; Q-1 first), 9 (low scud, perhaps)
---

# Render milestone 12: clouds

Design, tasks and checks of an open milestone; its order is in [PLAN.md](../PLAN.md#render),
the renderer it builds on in [render/](../render/README.md). On acceptance it is folded into
the subsystem's document there and this file is deleted. Format: [WRITING.md](../WRITING.md).

## Rulings

Decided by the owner (D-7): the levels' painted cloud layers are replaced by modern clouds,
not constrained by the game's approach; the sky is clear until then.

**Reachable clouds** (D-10): with Dagor's structure the clouds are volumes in the flight space,
flown under, into and above, and they may hide aircraft and the ground. The GPU budget is set
later (owner); until then it is measured, not capped.

## Design

- **Volumetric clouds.** Low and mid cloud in baked volumes (structure after Gaijin's Dagor
  daSkies2, below): Perlin-Worley shape and erosion noise, the cloud light table and sky as
  ambient, aerial perspective. Near clouds are marched from the camera per screen pixel at reduced
  resolution with temporal reprojection, stopped at the scene's depth; far ones come from a
  cloud panorama (2048x1024, read with a Catmull-Rom filter, and near the horizon, where a row is up to 8 times thinner than a texel is wide, averaged over a texel's width in rows so the march noise does not show as 1-pixel horizontal lines; the 192x108 sky-view table is too coarse for cloud edges),
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

- Stars and the night sky (D-12): the painted dome's stars and night clouds went with
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

**Tuned by eye** (D-11): the planned reference set (about 8 Poly Haven HDRIs, their
luminance ratios and cloud statistics) is dropped; the clouds are tweaked by eye in game.

The prototype (the two presets, the coverage value, the Mie phase and multiple scattering, before
the lighting is coupled) is judged on Kashan Desert from the ground, the air and inside a cloud at
coverage 0.3, 0.7 and 1.0, by eye. Its GPU time at
1920x1080 is measured on the benchmark paths; the budget is the owner's to set (the first plan's
0.3–0.6 ms was for the panorama alone).

## Built so far

### The cloud material

(`RaCloud.fxh`, shaders outside this tree). Density
in g/m³ of liquid water; extinction 3·LWC/D (D the droplet diameter in µm). Phase: Jendersie and
d'Eon's fit, delta-scaled: its HG part (g ≈ 0.995, under 0.1° wide, half the extinction:
the diffraction peak) is treated as unscattered, so extinction is times the Draine weight w
(0.50 at 20 µm) and the phase is the Draine lobe alone, evaluated directly (normalized, 1/sr).
Dagor keeps the spike, which would put about 2700/sr into a sub-pixel ring around the sun.
Multiple scattering: Dagor's three octaves (contribution 0.7, attenuation 0.3, eccentricity
0.6, weights normalized to 1); the first octave takes the droplet phase, the others a dual-lobe
HG; the light volume will store the last octave's transmittance and the include raises it to the
others'. Ambient: the sky from above and the ground from below, their mean over the sphere
plus the L1 band seen along the view ray through an effective anisotropy g = 0.6
(`Ra_CloudAmbientAlong`, contract 34; de-ringed per channel), so a cloud takes the colour of
the sky behind it. Frostbite's and Dagor's half desaturation is off: it greyed the dusk sky. A test effect compiles to 113 ps_3_0 slots.
### Cloud light

(`Atmosphere.cpp`, `buildCloudLight`, after Dagor's `clouds_light_color`),
built with the fit (adds about 25 ms): `RaCloudLightTable`, 64x32 `A16B16G16R16F`, managed. 8
altitude tiles across (layer i at (i + 0.5)/8 of 12 km above the reference height), 4 rows of
tiles down: the sun's irradiance (the sun above the atmosphere times the transmittance there),
the sky's mean radiance from above (its cosine-weighted irradiance / π, from the sky model at
that radius and sun cosine) and the ground's radiance from below (albedo times the ground's sun
and sky irradiance / π, through the air up to the layer); the fourth (contract 34), the sky's
first moment toward the sun's azimuth / π (∫ L sin θ cos φ over the upper hemisphere), which
with the irradiance is the sky's L1. A tile's 8x8 texels span ±280 km
around the level's origin, so the curvature lowers a far deck's sun. The haze is left out, and
the ground everywhere has the level's albedo. Kashan Desert (sun 50°): sun 7.3/7.4/7.1 at
0.75 km to 7.7/8.1/8.9 at 11 km; sky 0.07/0.15/0.36 to 0.02/0.04/0.12; ground 0.59/0.49/0.30.
`tools.atmosphere` dumps it (`[cloudlight]`). Nothing samples it before task 3.
### Weather and volumes

(`Clouds.cpp`, contract 13; Dagor's structure in D3D9 form). A weather
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
### The panorama

(`Clouds.cpp`, `RaClouds.fx` technique `RaCloudPanorama`, `RaCloudPanorama.fxh`,
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
  most 128, and never shorter than what is left over the steps left, so a march reaches its
  end), from a white-noise offset per texel and panorama, until the transmittance is under
  0.01. The panorama starts at 0.7 of the handoff and the near clouds' sky pixels end at it, the
  two densities cross-fading over that band; over the scene the near clouds march to its depth.
  The air in front of both is one integral, `Ra_AirAlong` (`RaAir.fxh`, contract 32), the aerial
  atlas's: the panorama per texel at the clouds' mean distance (its own closed-form haze, without
  the ground haze, left a step at the handoff and a radial edge far out). A step reads the coarse copy, and only where it bounds cloud the field, the detail noise
  and the light volume. Even steps with an interleaved-gradient offset (the first build) left
  a regular stripe pattern in the far deck. The cloud light table is read at the segment's two
  ends and blended. Light per step as `RaCloud.fxh`; the sun's share times an edge albedo by the
  view ray's opacity so far (Dagor's: 0.8 where the ray enters, back to 1 at sharpness 4, none
  toward the sun), and the light volume baked from the field eroded by the detail noise
  (contract 33). Both modest at dusk (Kashan Desert, `hb-owner`, `hb-owner-back`): outlines more
  defined, undersides still flat. Tried and dropped (D-30): a density powder term
  (dims evenly, as Dagor's notes say), and near taps 150 m toward the sun and up (Dagor ships its
  sun taps off): rounder bases, but not the look.
  Found (2026-11-23): at Kashan's dusk the flatness is the air, not the cloud light. Its
  weather (visibility 20 km, dust, mixing 3500 m: 0.18/km up to 4.7 km above sea level) puts a
  cloud 20–30 km off behind an optical depth of 4–5, so the haze's in-scatter is nearly all of
  it; without the air in front (a debug switch) the same clouds show lit tops, blue shaded
  sides and dark bases, and at `renderer.weatherVisibility 60000`, `weatherMixing 1500` they
  keep them. From the ground the bases are lit mostly by the ambient (sun share small), hence
  the view-dependent ambient (contract 34): with clear air the shaded sides away from the sun
  go cool blue-grey instead of warm grey; under Kashan's own haze it barely shows.
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
### Near clouds

(`Clouds.cpp`, `rendererCloudsNear`; `RaClouds.fx` techniques `RaCloudNear`,
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
reads as soft fog. Far clouds at dusk shading flat was the haze in front (above, 2026-11-23);
close up the light volume's ~250 m cells (Dagor's are the same) still hold no shape at the
detail's scale. Not measured: the GPU cost (the final GPU pass).
### Coverage drives the lighting

(task 7; `SkyCube.cpp`, `Atmosphere.cpp`, `Clouds.cpp`; contract 16).
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
is under the slab's top), and the clouds' light from
the ground below them. Past the panorama's 150 km reach the slab's clouds are extrapolated from
the marched part (to 400 km), so an overcast deck closes to the horizon. The sun on surfaces is
dimmed by the cloud shadow map (task 8), not by S, which takes over only past the map.
The sky behind the clouds and their gaps (contract 35, 2026-11-23): the sky view table is the
clear sky's, so its air under the deck was sunlit while the air in front of the clouds was in
their shadow, and gaps glowed past clouds veiled by the same dust (the owner's "the sky is
crystal clear against the clouds in the haze"). The sky pass took the shadow only within about
3° of the horizon. Now the main panorama, while the camera is under the slab, marches the air
from the camera to the slab's base (`Ra_AirCloudDelta`, `RaAir.fxh`: `Ra_AirAlong`'s in-scatter
less the clear sky's, 16 steps) and dims the sky behind by it, as a luminance factor in the
panorama's a; the sky pass's 3° blend is gone. A colour rest in rgb (the shaded air is bluer)
left bright fringes at cloud edges (the compression takes colour as opacity's) and was dropped.
Not in the sky cube's 256x128 panorama (`SkyCube.cpp` reads its a as the clouds' transmittance
toward the sun, which feeds S). Kashan Desert at dusk (views `hb-owner`, `hb-owner-back`):
gaps dim toward the veil, most away from the sun; at 60 km visibility, no visible change. Not
measured: the panorama's added cost (one 16-step march per texel, over its frames).
Kashan Desert (agent): at 0.4 S 0.99 and the sky light as clear; at 1.0 (desert preset, a deck of
towers 2–6 km) S 0.12–0.14, sky irradiance up 0.24/0.25/0.29 against the clear 0.09/0.18/0.39,
global horizontal about 17 % of the clear day's, the diffuse part 24 %. Against references:
the CIE overcast sky (horizon about a third of the zenith) is matched in shape, the horizon
darker than overhead; Kasten–Czeplak puts an 8/8 deck at about 25 % of clear and mostly diffuse,
so this coverage 1.0 reads as a storm deck (towers with optical depths in the hundreds), as the
coverage scale intends, darker and more direct than a stratus overcast. `renderer.clouds 0`
restores the clear sky's light (checked: the same irradiance as before).
### Cloud shadows

(task 8; `Clouds.cpp` `shadowStep`, `RaClouds.fx` technique `RaCloudShadow`,
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

## Tasks

What each built task did is under "Built so far" above.

### render-12.1 Cloud material and the cloud light table

State: built. `RaCloud.fxh` (delta-scaled droplet phase, octaves); the light table after Dagor's
`clouds_light_color`.

### render-12.2 Weather map and baked volumes

State: built. Contract 13; `renderer.cloudPreset`.

### render-12.3 Cloud panorama, high cloud and wind

State: built. Contract 14; `renderer.clouds`.

### render-12.4 Reachable clouds

State: built. Contract 15; `renderer.cloudNear` (D-10).

### render-12.5 Far cloud noise and shimmer

State: built. Tuned by eye (D-11).

### render-12.6 Prototype judged by eye

State: seen (2026-11-22). Kashan Desert from the ground, the air and inside a cloud at coverage
0.3, 0.7 and 1.0. GPU time with the final GPU pass (D-20).

### render-12.7 Coverage drives the lighting

State: built. Contract 16.

### render-12.8 Cloud shadows and the day sky

State: built. The cloud shadow map (Beer, Hillaire 2020; contract 16) and the clouds in the sky
cube. The night layers, once part of this task, are `render-12.10`.

### render-12.9 Low scud under overcast

State: proposed (perhaps).

### render-12.10 Night layers

State: planned; waits on Q-1. Stars from a catalog with the moon, the sky's rotation from latitude
and sidereal time (Dagor's `daStars`, `daSkiesAstronomy`); PR's sun direction kept, a date fitted
to it (D-12).

# Renderer: water

Part of [the renderer as built](README.md).

## 16. Water

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
  `renderer.waterWindFrom` or the open-water map's choice (milestone 23, task 2). The Falklands at the defaults: significant height
  1.17 m, mean square slope 0.026. Geometry stays flat (owner, 2026-11-22: normals only for now).
- *Filtered normals and the sun path* (task 3). The slope maps hold the slopes along and across the
  wind (published to the water shader as `WaterScroll`, contract 39) and their squares: in that
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

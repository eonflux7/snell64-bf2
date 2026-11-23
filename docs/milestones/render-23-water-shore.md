# Render milestone 23: water at the shore, water types, caustics, rivers, whitecaps

Design, tasks and checks of an open milestone; its order and state are in
[PLAN.md](../PLAN.md#render), the renderer it builds on in [render/](../render/README.md). On
acceptance it is folded into the subsystem's document there and this file is deleted.

Builds on milestone 16 ([water.md](../render/water.md)). What the owner sees (2026-11-23):
- Muttrah City, view `ssr-shore`: dark ragged patches with stepped edges along the promenade, tens
  of metres out. Screen-space reflections or the bed under the water.
- The shore's foam is a fixed white strip. Its depth threshold (Hs / 0.78, about 1.5 m at 7 m/s)
  is the open sea's everywhere, so shallow rivers (Kunar) foam over their whole width.
- Whitecaps look uniform and tiled over a wide sea: the 87 m cascade's foam is broken only by a
  40 m noise field.
- The water's colour: `renderer.waterColor` alone sets both the albedo (× 8) and the extinction
  (0.03 / albedo per metre), from a colour the levels authored for retail's blend.

Decided with the owner (2026-11-23): physically based throughout. No fade at the shoreline: the flat
plane meeting the terrain is the right edge. What is missing there is the run-up, wet sand and
breaking foam. The shore follows Dagor's approach (its open-source DagorEngine is read first).

Tasks:

1. **The dark shore patches:** built (shader only). Not the reflections (none with them off): the
   water body's refracted bed read. It stepped the slanted view path (up to 30 m) along the bent
   ray, far below the bed in shallow water, so the read landed on other depths, ragged with the
   waves. Now three fixed-point steps to the bed (its point on the read pixel's camera ray, its
   depth under this surface); the straight read where a read leaves the screen, meets sky or
   something above the water. The view's path in the water is the depth over the bent ray's
   descent (it was the unbent slant: oblique shallow water was too murky). Muttrah City
   `ssr-shore` clean, `seahigh` unchanged. The reflection march's weak spots (16 steps, hits on
   back faces) stay open until something shows them.
2. **Open-water map:** built (`WaterOpen.cpp`, contracts 38–39; `renderer.waterOpen` the A/B switch
   until acceptance). Baked on the first water frame after a level loads, and again when the sea
   or a given wind changes: the heights sampled on the main thread over the map's square at 1024²
   (2 m a texel on Muttrah City), the rest on a worker (1.2 s there). Per texel: the signed distance to the shore
   (two exact Euclidean distance transforms of the water mask, height under the sea level), the
   direction toward open water (its gradient), and the effective fetch upwind: one sweep per
   radial at 6° steps within 90° of the wind, each radial's fetch (at most the level's) turned
   into the fetch-limited JONSWAP sea's energy (∝ F^1.11), averaged with the spectrum's spreading
   weighted by energy, and turned back into a fetch. Along each radial the waves diffract: what
   arrives from upwind spreads across the ray as it travels (the parabolic approximation, a
   diffusion of the energy across each row of the sweep, solved implicitly; variance λ_p x / 4,
   the Fresnel edge's width, λ_p the level's peak wavelength, 34 m at 7 m/s), so a breakwater's
   shadow is sharp at its tip and blurs downwind. (The first build averaged the fetches, the
   Shore Protection Manual's effective fetch: one open radial carried a whole sea, a hard line
   from a pier's corner; energy without diffraction still left the radials' knife-edged shadows
   as steps.) Past
   the map, water along an edge in a run of at least 1 km is open sea, a shorter run a shore (a
   river leaving the map).
   The wind blows from the map's most open sea (owner, 2026-11-24: physical; retail's
   `WaterScroll`, a texture scroll, is no longer the wind): of 60 directions, the one bringing the
   most wave energy to the water, a sea breeze (a warm coast's daytime wind); on a river map along
   its longest reach. `renderer.waterWindFrom` overrides it with a bearing (0 from +z, 90 from +x;
   −1 automatic). The live spectrum's direction reaches the shader as `WaterScroll` (contract 39).
   The clouds keep their preset's wind for now.
   With each spectrum, a table over log2 fetch (16 m to 512 km, one entry an octave) from the same
   wind's JONSWAP integrated over each cascade's band: each cascade's slope amplitude against the
   level's, the significant height's ratio, and the whitecap coverage's ratio from wave age
   (Zhao and Toba 2001: coverage ∝ ω_p^−1.09, so fetch^0.36; re-thresholding the foam maps by the
   steeper young sea would give short fetch more whitecaps, not fewer). The water shader scales
   each cascade's slopes and variance by its amplitude, the crests' foam by the whitecap ratio,
   and the shore's breaking depth by the height ratio (at least 2 cm). At 7 m/s: under 1 km of
   fetch no 503 m swell and little of the 87 m cascade, the finest steeper (×1.45 at 1 km, young
   seas), the height ×0.1. `tools.water` dumps it. Muttrah City `ssr-shore`: wind from 60°
   (east-north-east, onshore), mean effective fetch 22 km, the bay's lee smooth from the
   breakwater's tip; the long breakwater's lee stays calm right behind it. Kunar's rivers not yet looked
   at (task 9).
3. **Shore waves (Dagor).** The spectrum's waves feel the depth from the distance map: shorter and
   slower in shallow water (the finite-depth dispersion, ω² = g k tanh(k d)), turning toward the
   shore's normal (refraction), and lower where the water is too shallow to carry them (a
   depth-limited, TMA-style spectrum), so the open sea's pattern no longer reaches the waterline
   unchanged. On top, wave fronts travelling up the distance map's gradient, breaking where the
   depth falls under their height, foam from the break; run-up sheets on the beach and a wet sand
   band on the terrain above the waterline. Replaces the fixed shore strip.
4. **Water types.** Pure water's absorption built in, and turbidity (scattering and its tint) as a
   water type from Jerlov's classification, chosen from where the level is set (the Gulf of Oman
   for Muttrah City, a glacial river for Kunar), not fitted to retail. `renderer.waterColor` is
   no longer read (owner, 2026-11-24: physically correct, not judged by retail). Each level's type
   is map data in the mod's `settings/`, like its views, with a console variable to override it
   and a clear coastal type when a level has none. Rivers get their own type.
5. **Surf turbidity.** Breaking waves stir up sediment: the water's scattering rises with task 3's
   breaking energy and falls with distance from the break, the brown band along a real beach,
   in the sediment's tint over the water type's.
6. **Caustics.** The sun focused by the waves onto the bed in shallow water, from the slopes the
   FFT already makes (the refracted sun's convergence), fading with depth as the focus blurs and
   with the water type's extinction; on the terrain and objects under the water.
7. **Rivers.** A current: the waves' ripples and foam advected downstream, the current shifting
   the spectrum's frequencies, standing waves and foam behind rocks and banks. The water is one
   flat plane per level, so the flow's line comes from the channel's axis in the open-water map
   and its sense (downstream) is map data in the mod's `settings/`, like the water type; its
   speed from the channel's width.
8. **Whitecaps.** Wind gusts varying over hundreds of metres, and breaking weighted by the 503 m
   cascade's crests too, so no scale reads as a tile.
9. **Checks.** Muttrah City `ssr-shore`, `seahigh`; The Falklands; Kunar's rivers; GPU cost.

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
3. **Shore waves (Dagor).** Built: the train and its breaking (contract 40), the swash up the beach
   and wet sand (contract 41), 2026-11-24. Dagor (`fftWater/shaders/shore.dshl`) runs one Gerstner train phased by a
   shore distance field, faded by depth and distance, foam on a sawtooth of its phase; here the
   phase is physical instead:
   - **Shore map** (`WaterOpen.cpp`, beside the open-water map in a 2048-wide texture): the depth
     under the sea level, and the phase of the level's peak wave (4.7 s at 7 m/s) solving the
     eikonal equation |∇φ| = k(d) by fast marching from the deep water (k0 d > π), where it is the
     open sea's plane wave from the wind. Crests are its level sets: they shorten in shallow water
     and turn parallel to the shore (refraction), wrap round a breakwater's tip, focus over a shoal
     (Omaha's). Stored modulo 8π; the shader filters four texels, unwrapping each. The land ring at
     the waterline carries the phase and direction on. `tools.water` writes `-shore.png` (the crests,
     brighter shallower). The bake grows to 0.9–1.4 s.
   - **Shader** (`Ra_ShoreWaves`): where the bed is felt (k0 d < π), the train on the phase: k from
     the dispersion, height the sea's rms (0.71 Hs × the fetch's ratio) × shoaling (√(cg0/cg)),
     in groups of four waves and a field along the crests (two cross-fading every 50 s), limited to
     0.78 d (breaking; the surf zone stays at it). Peaked and leaning forward as it nears breaking.
     Its slopes add to the FFT's; past four pixels a wavelength they become variance. The FFT's
     503 m cascade (the peak's band) hands over to the train as the bed is felt, the finer two
     lose their low frequencies by the TMA depth factor, and the random sea is depth-limited.
     Foam from the breaker's crest, trailing behind it, longer in the inner surf; it replaces the
     fixed strip (kept with `renderer.waterOpen 0`).
   - Seen: Omaha Beach `surf`, `surf-along` (bands of broken waves moving up the beach); Muttrah
     City's shores drop steeply (quay walls, rocks: foam only at the waterline), `beach-e` the cove.
   - **Swash** (contract 41; `RaShore.fxh`, shared by the water and the terrain so both evaluate
     the same edge): each wave's bore runs up the beach and drains back. Per wave the run-up and
     run-down levels from Stockdon et al. (2006) (setup 0.35 tan b sqrt(H L0), swash range
     sqrt(H L0 (0.563 tan² b + 0.004)), × 1.1; the slope capped at 0.2, the run-up at H, where
     steeper shores reflect), H the train's height for that wave (its group, the field along the
     crests, and lobes every 8 m along the shore, one of four fields per wave, so each bore reaches
     its own lobed line). The shoreline moves ballistically between them (g sin² b, or faster so a
     swash ends within three periods), the last four waves overlapping: a new bore overruns the
     last one's backwash. The terrain draws, above the still water, the sheet (wet ground through
     pure water's absorption, the sky cube's and the sun's reflection by Fresnel, the bore's foam
     as lace); behind the backwash a draining film (glossy, 1.5 s); below the group's largest
     run-up damp ground (albedo × 0.45, saturation × 1.25). The water clips its own plane where the
     ground it shows lies above the swash level, so the run-down uncovers the bed. The bake carries
     the waterline's phase, direction and fetch up the beach to 4 m above the sea (128 texels).
     Seen: Omaha Beach `swash` (at the waterline, facing the sun), `surf-along`; Muttrah City
     `beach-e` (the rocks: a thin wet band).
   - Open: the deep-water start is everywhere deep, so a harbour behind a breakwater starts its own
     plane wave from the wind (its height is the fetch map's, small).
4. **Water types:** built (`WaterMedium.cpp`, contract 42, 2026-11-24). The medium is pure water
   plus three constituents, each from a published bio-optical model: pure water's absorption (Pope
   and Fry 1997) and scattering (Morel 1974); chlorophyll's phytoplankton absorption (Bricaud et
   al. 1998) and particle scattering (Gordon and Morel 1983, backscattered by Morel and
   Maritorena's ratio); dissolved organic matter's absorption (CDOM, e^(-0.015 (λ - 440)));
   mineral sediment's absorption and scattering (Babin et al. 2003, backscattered 1.83 %). Per
   screen channel (linear sRGB's response from the CIE observer): the diffuse attenuation K = a +
   b_b, a channel's mean transmittance over 3 m, and the backscattering b_b, published as
   `RaWaterMedium` (c176–c177). The water body attenuates every leg by K (forward scattering
   stays on the path; the bed's blur is not modelled) and scatters 2 b_b isotropically toward the
   eye: deep water's reflectance within about 10 % of Gordon's 0.089 b_b / (a + b_b). The swash's
   sheet takes the same K. `renderer.waterType` picks a preset: 0–4 oceanic I, IA, IB, II, III,
   5–9 coastal 1, 3, 5, 7, 9, each set near Jerlov's diffuse attenuation at 450, 550 and 600 nm,
   10 a glacial river (30 g/m³ of rock flour); `renderer.waterChlorophyll`, `waterCdom`,
   `waterSediment` set one constituent (the type reads −1). A level's type is in its weather file
   (`settings/eon64-weather/<level>.txt`, `waterType`); without one, coastal 1. Muttrah City
   coastal 1 (green), Omaha Beach coastal 5, The Falklands coastal 3, Kunar the glacial river
   (milky grey). `tools.water` writes the medium. `renderer.waterColor` (`WaterColor`) is no
   longer read by the water body; only the path without the scene copy (under water) keeps it.
   Seen: Muttrah City `ssr-shore` (types 0, 5, 9: blue, green, olive); Kunar `river` (the glacial
   type against oceanic II). Open: glacial flour's turquoise (the sediment's scattering slope is one
   for all minerals); the terrain's far water colour (`TerrainWaterColor`) is the level's.
5. **Surf turbidity:** built (contract 43, 2026-11-24). Breaking waves hold fine sediment in
   suspension: a concentration (g/m³) added to the water type's sediment, with the same mineral's
   coefficients (`WaterMedium.cpp` publishes what each g/m³ adds to K and b_b: the medium with
   50 g/m³ more, less the medium, over 50; `RaWaterMedium[2..3]`). The shader (`Ra_SurfSediment`,
   `RaShore.fxh`): the breakers' height Hb from the train's deep-water height and period (Komar
   and Gaudiano 1975, Hb = 0.39 g^0.2 (T H0²)^0.4), breaking at Hb / 0.78; in the surf zone
   `renderer.waterSurfSediment` g/m³ per metre of Hb (40; 0 none; at most 150), none under 5 cm
   breakers; offshore of the break an e-fold over 0.3 to 1.2 breaking depths, varied by plumes
   (cells along the shore of 100 breaking depths, three surf widths on a 1:30 beach, stretched
   offshore, changing over 500 s); only on sand (none where the bed is steeper than 0.25, full
   under 0.08). The water body takes the pixel's concentration for its whole path; the swash's
   sheet takes it in its K. Omaha Beach (Hb 0.98 m, 39 g/m³): the surf zone khaki and opaque where
   it was clear green, `surf-along`, `surf-plume` (from the sea, the sun low behind: the
   obstacles' shadows now show on the turbid water as shafts). Muttrah City `beach-e` unchanged
   (rocks and quays). Open: Omaha's flat is shallow for hundreds of metres, so the whole of it
   is surf zone and the plumes hardly show; a rectangular shadow edge on the turbid water at
   `surf-plume` (the shadows' cover, not this task's).
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

# Tools: renderer dumps

Part of the `tools` lane ([tools.md](tools.md)); the dumps `tools-4` (renderer inspection)
builds on, built ahead of it. Output goes under `tools.outputDir`.

## Survey

`tools.survey [NAME]` (render milestone 11, task 1; `tools(survey)`) writes
`<outputDir>/<map>/survey/<NAME>.txt`, NAME defaulting to the held view's name (or `camera`);
`tools.lastSurvey` gives the result. The request waits for the next frame: an occlusion query wraps
that frame's water flush (`RSMTWater`), and once its result is in, the renderer
(`RendDX9/ToolsSurvey.cpp`) writes the whole report in one go. Idle, it costs one flag test at the
water flush and one after Present. Run it once at each reference view: the level sections repeat,
and the water area is the view's own.

One line per fact, tagged by section:
- `[camera]`: whether the camera is under water (then the fog constants hold the water fog).
- `[sky]`: dome template, texture and cloud textures, lighting colour and blend, the effect's
  current technique, and an 8x8 grid of the dome texture's mean colours (top row first).
- `[fog]`: the console fog colour, start, end and base decoded from the `FogRange` constant, and
  the water fog.
- `[light]`: sun direction, elevation (`asin(-y)`) and azimuth; static and dynamic sun, sky,
  specular, ambient, high/low, effect and tree colours.
- `[terrain]`: file and patch heights, colormap formats, the colormaps' mean colour (a level at most
  16 wide, averaged over patches).
- `[water]`: the four water techniques, the water env map (and whether it is the level's
  `Envmaps/EnvMap0`), and water's share of the screen's samples.
- `[envmaps]`: the env-map system's switches, maps (position, texture) and templates.
- `[bundled]`, `[static]`: loaded templates, and materials grouped by technique, alpha mode, state
  flags (normal, env, colour-map gloss, alpha test, cockpit) or combo bits, with the range of
  specular power, static gloss and reflectivity and example diffuse textures.
- `[normalmaps]`: per normal map (first 40), the mean length of the decoded normal per mip level
  from the first level at most 256 wide; lengths that fall with the level mean the mips were not
  renormalized.
- `[effects]`: per compiled effect (legacy and Ra caches), the pixel and vertex sampler registers
  its shaders use (from their constant tables), cube samplers and cube texture parameters; then
  how many effects use each pixel sampler.

Textures are read from their managed copies (DXT1–5, A8R8G8B8, X8R8G8B8); default-pool textures
and other formats are reported unread.

## Atmosphere

`tools.atmosphere [NAME]` (render milestone 11, task 2; `tools(atmosphere)`) writes
`<outputDir>/<map>/atmosphere/<NAME>.txt` at the console call, NAME as for `tools.survey`, from
`RendDX9/ToolsAtmosphere.cpp`: the atmosphere's fit and its tables as drawn, for reading without
the debug menu. Fails before the first fit.

- `[fit]`: fit time, reference height, fog end; the authored horizon over the model's (not
  applied); sunlight above the atmosphere; the zenith model and the horizon target against the
  model; ground albedo; haze density and albedo. `[atlas]`: its format and the
  vertex texture fetch check.
- Per table (`transmittance`, `multiscatter`, `skyview`, `perspective`): size; per channel min,
  max, mean and the count of non-finite values; a PNG `<NAME>-<table>.png` with rgb scaled to the
  table's maximum (the scale is in the line); samples on a grid, each row's and column's meaning
  given.
- `[perspective]`: also `<NAME>-perspective-transmittance.png`, and per slice its distance and,
  at four tiles (screen centre, top centre, left and right of the middle row), in-scatter rgb |
  transmittance.

- `[skycube]` (render milestone 11, task 7): live or not, build time, the irradiance's sky scale
  and its upward value, the 7 `RaSkySH` rows; per mip each face's mean rgb (and bad texels); a PNG
  `<NAME>-skycube.png` (faces +x −x +y −y +z −z across, mips down, upscaled, rgb scaled to its
  maximum); irradiance / pi at 8 normals from `RaSkySH` against a cosine integral of the top mip,
  with their ratio. `[brdf]`: the split-sum table's statistics, `<NAME>-brdf.png` and a grid.
- `[cloudlight]` (render milestone 12, task 1): the clouds' light table's statistics and
  `<NAME>-cloudlight.png`; per altitude (0.75–11.25 km), at x −245, −35 and +245 km, the sun's
  irradiance, the sky's radiance from above and the ground's from below.

The managed tables are locked where they are; the atlas is read back from its render target.

## Exposure

`tools.exposure [NAME]` (render milestone 13; `tools(exposure)`) writes
`<outputDir>/<map>/exposure/<NAME>.txt` at the console call, NAME as for `tools.atmosphere`, from
`RendDX9/ToolsAtmosphere.cpp`: the HDR post as the last frame left it. Fails before it has run.

- `[settings]`: the `renderer.exposure*` and `bloomStrength` values, the adaptation rates
  and the key. `[frame]`: frames run, whether the last ran (or thermals skipped it), its dt and
  whether it snapped, the scene's size.
- `[state]`: adapted and metered EV (and luminance), the exposure as a multiplier and in stops.
- `[histogram]`: each non-empty bin's low EV, weight, share and cumulative share.
- `[meter]`: the 64 x 32 log2 luminances' range and mean, and a sparse grid of them.
- `[bloom]`: level 0 after the up chain, statistics and `<NAME>-bloom.png`.

## Sun

`tools.sun [NAME]` (render milestone 14; `tools(sun)`) writes `<outputDir>/<map>/sun/<NAME>.txt`
at the console call, NAME as for `tools.atmosphere`, from `RendDX9/SunStatic.cpp`: the sun's static
layers as last drawn. Fails before they are drawn.

- `[settings]`, `[sun]`: the `renderer.sunStatic*` values, the sun they were drawn for.
- `[RaSunStatic i]`: the drawn rows (the per-frame offsets and flag excluded).
- `[static]`: the light depth's range, the share of texels with a caster, `<NAME>-static.png`
  (1024 wide, white = none). `[height]`: the shadow height's range, `<NAME>-height.png`.

## Water

`tools.water [NAME]` (render milestone 23, task 2; `tools(water)`) writes
`<outputDir>/<map>/water/<NAME>.txt` at the console call, NAME as for `tools.atmosphere`, from
`RendDX9/WaterOpen.cpp`: the open-water map as last baked and its fetch table. Fails before the
first bake; while a re-bake runs (a level load, a changed sea or current) it writes the last one.

- `[map]`: its size, the texel, the sea level, the wind it was baked for, whether it is bound.
  `[bake]`: the water's share of the map, the edge runs taken as open sea, the bake's time.
- `[fetch]`: the water's share by effective fetch (under 100 m to open sea).
- `[table e]`: per entry its fetch, the three cascades' amplitudes, the height and whitecap ratios.
- `<NAME>-distance.png`: the signed distance (grey the shore, white 256 m out, black 256 m inland);
  `<NAME>-fetch.png`: log2 of the fetch over water (16 m black, 64 km white), land black. North up.
- With a current (task 7): `[flow]` the solve, `[obstacles]` the water texels rocks and piers stand
  in, `[standing]` the flowing water's share by Froude number, where its standing waves break and
  the mean foam carried. `<NAME>-flow.png` the speed (white twice the mean), `<NAME>-flowfoam.png`
  the foam carried, `<NAME>-obstacles.png` the share of each texel's section closed (grey open
  water, white closed), `<NAME>-potential.png` the flow's potential in bands 1/64 apart.

## Clouds

`tools.clouds [NAME]` (render milestone 12, task 2; `tools(clouds)`) writes
`<outputDir>/<map>/clouds/<NAME>.txt` at the console call, NAME as for `tools.atmosphere`, from
`RendDX9/ToolsAtmosphere.cpp`: the sky clouds' live volumes. Fails before the first set is live
(about 25 frames after the atmosphere's fit).

- `[clouds]`: preset, coverage, the weather's life time, the slab, LWC and droplet size; the
  noise's and the weather's build time, frames per build, builds so far.
- `[weather]`: statistics and `<NAME>-weather.png`. `[shape]`, `[detail]`: the noise volumes'
  slices, `<NAME>-shape.png`, `<NAME>-detail.png`.
- `[field]`: per slice its height, the share of texels over 0.02, the mean and the maximum; from
  above, the share of the sky whose column optical depth (RaCloud.fxh's extinction) is over 1 and
  over 10, its mean and maximum; `<NAME>-field-columns.png` (each column's maximum) and
  `<NAME>-field-slices.png` (every 4th slice). `[coarse]`: field texels above their block's bound
  (0 expected).
- `[light]`: per slice the mean sun (last octave), sky and ground visibility, and the darkest sun;
  `<NAME>-light.png` (every 4th slice, sun and sky visibility).

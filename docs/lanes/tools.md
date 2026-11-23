# Tools

The `tools` lane: an in-game test harness for renderer work, called the **view tool**. It replaces
the manual loop (load, spawn menu, random spawn point, fly to the spot, look, screenshot) with
saved views, view sets, camera paths and unattended runs that write a report. Rules:
[AGENTS.md](../../AGENTS.md#lanes).

- **Console first.** Everything the tool does is a console command or variable. The **Views** tab
  of the debug menu ([ui.md](ui.md)) is one frontend; `BF2_DEVCMD_FILE` ([dev.md](dev.md)),
  scripts and config files are the others. An agent and the owner drive the same commands.
- **Results are files.** Screenshots, a JSON report and a short Markdown summary per run, so a run
  can be read without watching it. Diffs and contact sheets are made from those files.
- **Idle costs nothing.** While no view, path or run is active, frames draw and cost the same as
  without the tool.
- **Map data is the owner's.** View files live in the mod's `settings/` directory, outside this
  tree, like `eon64-commands.txt`. Debug shading modes live with the shaders, outside this tree; the
  engine only provides the variable that selects them. Run output (`tools.outputDir`) also lies
  outside this tree: it holds screenshots of the mod's content and the shader set's commit.

**Names.** Commands are grouped by what they act on: `view.*` (views, sets, shots, runs),
`path.*` (camera paths), `bench.*` (benchmarks and the perf map). The tool's own settings are
`tools.*` variables. Switches that change what the renderer draws (debug shading, compiling all
effects) are `renderer.*` variables, since the renderer owns them. All names below are provisional.

**Flags.** `BF2_VIEWRUN` (milestone 2) is the only environment variable planned. It is permanent,
off by default and inert when unset, and is listed in [dev.md](dev.md) when it lands.

## Milestones

Order and state: [PLAN.md](../PLAN.md#tools).

### 1. Views

**Camera override.** The tool takes the render camera after the game sets it and before culling,
so culling, LOD, streaming and shadow cascades follow the override. No soldier is spawned: it
works from the spawn screen, with the spawn menu and HUD hidden while a view is active. Leaving the
view hands the camera back to the game.

Only the client's camera moves. The server still places the player at the spawn screen, so dynamic
objects (vehicles, other soldiers) far from it may not be sent to the client and do not appear at
a view. Static geometry, terrain, overgrowth and effects are local and always draw. Views that need
dynamic objects wait for soldier mode (milestone 6).

**Free fly.** A key toggles a free camera: no collision, mouse look, adjustable speed, mouse wheel
for FOV. This is how views are placed.

As built (`Game/Tools/ViewTool.cpp`): `tools.flyKey` (Win32 virtual-key code, default 119, F8) or
`view.fly 1` starts from the game's current camera. Free fly reads the keyboard and mouse itself
once per frame, not the player input: the spawn screen's control map has mouse look but no
movement, and player input arrives at tick rate. W A S D move, E or Space up, Q or C down, Shift
5x, Ctrl 0.2x; the mouse looks (`tools.flyMouse`, degrees per pixel, default 0.15) with the
DirectInput mouse released and the cursor re-centred; the wheel changes the FOV by 2 degrees.
Input is read only while the game window is in front and neither the debug menu nor the console
window is shown (the cursor shows for them), and the game's player input is cleared meanwhile. `tools.flySpeed` is metres per second (default 20). Free fly started from the game's
camera hands it back when it stops (`view.fly 0`, F8); started from a view, it stops in place and
keeps the camera there, ready for `view.save`. `view.leave` always hands the camera back and
restores `renderer.drawHud`.

Engine points the tool relies on:
- Hook: `BF2Render::render` (`Game/Main/BF2Render.cpp`), after `setFarPlane` and before the
  `renderFrame` loop; everything that culls, picks LODs, streams or sets up cascades reads the view
  after it. On the spawn screen the camera is the player's `MultiPlayerFreeCamera`; with no player
  nothing sets the view, so the override does not depend on one.
- `RenderView::setViewMatrix` takes camera-to-world (rows right, up, forward, position; y up,
  left-handed); `setFov` is vertical, radians; `setAdjustedFov` widens for widescreen.
- HUD and spawn menu are both gated by `renderer.drawHud`.
- Pause: `ServerGameLogic::setPause` (game status 4) stops triggers, objects and physics and pauses
  particles; it has no console binding. Renderer time is one input, `renderFrame`'s `time`, so a
  freeze passes 0 there while a view is active.
- `renderer.dumpScreen` reads the back buffer at end of scene, after the HUD and before ImGui.

**A view** is a name, position, yaw, pitch, FOV, and options:
- `run` lines, executed on entering the view (debug mode, a forced cascade, a time of day). Before
  a line sets a console variable, the tool records the variable's value and restores it on leaving
  the view. Lines that are commands rather than variable assignments (a reload, for example) are not
  undone; the menu marks them;
- a note (for example "fence alpha flicker, check after render milestone 10"), shown in the menu and copied
  into reports;
- settle frames (`tools.settleFrames`, default 8).

**A set** is an ordered list of views with a dwell time each. Each map has one view file,
`<mod>/settings/eon64-views/<map>.txt`, where `<map>` is the level's folder name. A view applies to
every game mode and layer of the map; the mode and layer a shot was taken on are in its stamp.
Lines starting with `#` are comments. `muttrah_city_2.txt`:

```
view market   pos=812.4,62.0,-311.9 yaw=134.5 pitch=-8 fov=75 | rooftop alpha fences
  run renderer.sunShadowDebug 1
view airfield pos=-1203.0,48.5,902.2 yaw=12.0 pitch=-3
set default market airfield
set quick   market
```

`view` takes `key=value` fields. `pos` is `x,y,z` in world metres with y up, as BF2 stores
positions. `yaw` and `pitch` are in degrees, in the game's own camera convention (pitch is negative
looking down). `fov` is the vertical field of view in degrees and defaults to the game's. Indented
`run` lines belong to the view above them; `| text` is the note; `set NAME VIEW...` lists views in
order; `#` starts a comment.

**Editing.** The file is edited by hand (by the owner or an agent) and from the menu, so neither
may overwrite the other. Before each write the menu rereads the file, and if it changed since the
last read, reloads it first. It then rewrites only the lines of the views and sets that changed,
keeping comments, order and blank lines. `view.reload` (Reload in the tab) rereads the file.

**Commands:** `view.go NAME`, `view.save NAME [note]` (the current camera), `view.delete NAME`,
`view.move NAME up|down`, `view.note NAME [note]`, `view.set NAME ["VIEW..."]`, `view.leave`,
`view.list`, `view.reload`, `view.fly 0|1`, `view.shot [NAME]`, `view.lastShot`.

As built (`Game/Tools/ViewFile.cpp`, `ViewTool.cpp`): the file is reread whenever it differs on
disk from the last read, so the next view command after a hand edit sees it; `view.reload` forces
it and reports the counts. An edit changes only the lines of its view or set: `view.save` on an
existing view rewrites its `view` line (camera fields replaced, unknown fields and the note kept
unless a new note is given) and leaves its `run` lines; a new view goes after the last view.
`view.delete` removes the view's lines and drops it from every set, removing a set left empty. Line
endings are kept. Lines that do not parse are kept and listed by `view.list` and `view.reload`.
`view.save` needs a camera held by the tool (free fly or a view). A note with spaces is quoted:
`view.save market "rooftop alpha fences"`. A `run` line counts as setting a variable when its first
word names a console property and a value follows; the first such line per variable records the
old value, and `view.leave` or the next `view.go` restores them, last first. Commands are marked
"not undone" in `view.list`. A view without `fov=` takes the game's FOV.

**Views tab:** the list with Go, Update (to the current camera), Delete and reordering; set editing;
Save current view; the note and run lines of the selected view.

As built (`RendDX9/UiDebug.cpp`, read through `IUiViewTool` in `RendMisc/IRendererUi.h`): the
tab only reads the tool's state; every change is a `view.*` line run through the console, and its
result shows under the Save row. It rereads the file at most twice a second while shown. Free fly
pauses while the menu or the console window is shown, so it flies once the menu closes.
`view.move` swaps the view's lines (view line through last run line) with its neighbour's, leaving
lines between them in place. `view.note` with no note removes it. `view.set NAME "a b c"` replaces
or adds the set (after the last set); `view.set NAME` alone removes it. Run lines are shown, not
edited: they are added by hand.

Paths in the tab (milestone 3): the list (`>` marks the one playing) with key count, length and
note; double-click or Play runs `path.play`, Stop `path.stop`; Add key runs `path.key NAME [TIME]`
with the name and optional time fields (selecting a path fills the name). The selected path's
keys are shown as their file lines, edited by hand. The camera line reads "Path NAME, T s" while
one plays.

**Screenshot in the engine.** The scene is read back before the HUD (or with it, by option) and
written as PNG, plus a 960-wide JPEG for quick review. It works when the window is covered.

As built (`ViewTool.cpp`; renderer side `RendDX9/ToolsCapture.cpp`, reached through
`IRendererTools` in `RendMisc/IRendererTools.h`, queried from the renderer like `IRendererUi`):
`view.shot [NAME]` writes `<tools.outputDir>/<map>/shots/NAME.png` and `NAME.jpg`; without NAME
the name is the held view's (or `camera`) and the local time, and an existing NAME is overwritten.
`tools.outputDir` defaults to `eon64-runs` under the game's working directory (`@HOME@` may start
it); the directory is created. The next frame's hook passes the request to the renderer, which
copies the back buffer with StretchRect into a single-sample target (resolving MSAA), reads it
back and writes the PNG (RGB, no alpha) and a box-filtered JPEG `tools.shotWidth` wide (960). It
reads back after the composite and the sky's second pass, just before the HudManager paints; with
`tools.shotHud 1` it reads at end of scene instead, with the HUD and without the debug menu and
console, and while a view hides the HUD it is shown for two frames and hidden again. The command
returns once queued; the result goes to the console and `view.lastShot` (`wrote PATH (WxH, ...)`,
`failed: ...`, `pending PATH` or `none`). The renderer holds a request while the weather's tables,
the sky cube or a cloud build started by a setting are being rebuilt, and 24 frames after (the
cloud panoramas' redraw), so a shot right after a console change shows it. A request no frame
serves within 600 frames fails. The
encode is synchronous, so the frame of the shot is longer. Works with no view held.

**Settle and freeze.** Before a shot the tool waits until streaming has no pending loads and LOD
crossfades have finished, then `tools.settleFrames` frames, which also covers any shadow cascade
that is updated less often than every frame. If the wait exceeds `tools.settleTimeout` (default
10 s), the shot is taken anyway and the report names the condition that was still pending. While a
view is active, the renderer's time inputs are frozen: wind on overgrowth, water, particle updates,
the sun. Whether the game simulation can also be paused on a local server is part of this
milestone; if it cannot, the report says what was not frozen.

**Stamping.** Every shot and report records the engine commit, the shader set's commit, map, game
mode, layer, resolution, MSAA, vsync and frame cap, and the `renderer.*` variables that differ from
their defaults.

Accepted when: views are placed with free fly and saved from the menu, `view.go` from the console
and from `BF2_DEVCMD_FILE` reaches the same image, and two shots of the same view in one session
differ by no more than a stated tolerance (the **determinism check**), with every remaining source
of difference named in the report. Render milestone 8's anti-aliasing has no temporal part, so each
difference has a source that can be named. Bit-identical shots are milestone 5's goal, not a
condition here.

### 2. Unattended runs and regression

**Run.** `view.run SET [label]` visits each view of a set, settles, writes the screenshot and the
GPU pass timings, draw calls and primitives for that view, then a report. `BF2_VIEWRUN=<set>[:label]`
does the same right after autohost and quits when done, so one launch line runs a set unattended.

**Measurement**, the milestone's first task. The GPU pass timers and draw counters (`GpuTimers.cpp`,
`DrawStats.cpp`, today the `dev` flags `BF2_GPU_TIMERS` and `BF2_DRAW_STATS`) become this lane's
(owner, 2026-11-19). Both switch on at run time while a run, a benchmark or the overlay needs them,
and cost nothing while off; draw stats gain the live switch the GPU timers already have. A per-view
snapshot feeds the reports, and milestone 4's draw breakdown builds on the same counters. The TSV
files become `tools.gpuTimersFile` and `tools.drawStatsFile` (a path turns that file on, empty turns
it off), and both environment flags are deleted in the same change.

**Errors.** A set that names a missing view, a map without a view file, or a `run` line that fails
is recorded in the report as an error for that view; the run continues with the next view. The run
fails if any view has an error.

**Report.** In `<output>/<map>/<label>/` (`tools.outputDir`, default `eon64-runs` under the
game's working directory; single shots go to `<output>/<map>/shots/`): the PNGs and JPEGs, `report.json` (the stamp, per view: pass timings,
frame time, draw counts, settle results, the load gate, errors) and `report.md` (the same, short).

**Load gate.** During the load the tool counts shader compile errors, missing techniques and effects
that failed to load; they head the report and fail the run. `renderer.compileAllEffects` compiles
every technique of every loaded effect, not only those the first frames use (night variants,
vehicle-only and cockpit shaders), and logs each failure.

**Baselines and diffs.** A label marks a baseline (for example `m8`, `stock`). A script in this
repository compares two runs: per view a difference metric with a tolerance, a heatmap, and the
change in pass timings and draw counts; it writes one contact sheet image (thumbnails and heatmaps)
and a summary table. One image per comparison is what gets reviewed. The script refuses to compare
runs whose resolution or MSAA differ, rather than produce a heatmap of the difference.

**Stock baseline.** The same set run with the stock PR shader set gives a reference for drift from
the original look, alongside the previous milestone's baseline.

**Reload and diff.** `view.reloaddiff` shoots the current view, reloads the shaders, waits the
settle frames, shoots again and writes one A/B image with a heatmap: the shader edit loop in one
command.

**Watchdog.** A watchdog thread treats 10 s without a presented frame as a hang. On a crash, hang
or device loss, the last report entry names the view and command that were running and, for a
crash, the faulting address with its module and module base. The game does not ship map files, so
the diff script symbolizes the address afterwards from the build's RendDX9 map file.

Accepted when: an unattended run on Muttrah City and on Saaremaa produces a report and contact
sheet, a second run diffs clean against the first, and a deliberate shader error fails the load
gate.

### 3. Benchmark path

**A path** is a list of keyframes (view fields plus a time), played on a Catmull-Rom spline for
position and smoothed for angles and FOV. It is stored in the map's view file as `path NAME` with
indented `key TIME pos=… yaw=… pitch=… fov=…` lines. `path.key NAME TIME` appends a keyframe at the
current camera; otherwise the file is edited by hand. Paths play in real time: `path.play NAME`,
`path.stop`.

As built (`ViewFile.cpp`, `ViewTool.cpp`): `key` lines belong to the `path` line above them (as `run`
lines to their view) and must rise in time; a key that does not is listed as a problem and skipped.
`path` takes an optional `| note`. `path.key NAME [TIME]` needs a held camera (free fly or a view);
without TIME the key goes at 0 for a new path and 5 s after the last key otherwise; a key at the
same time (to 0.01 s) is replaced, another goes where its time puts it. A new path goes after the
last path, else after the last view. `path.list` prints the paths and their keys; `view.reload`
counts them. `path.play NAME` needs two keys; time runs from the first frame rendered after the
command. Position, yaw, pitch and FOV follow one cubic Hermite curve through the keys, tangents
`(p[i+1] - p[i-1]) / (t[i+1] - t[i-1])` (Catmull-Rom for uneven key times), one-sided at the ends;
yaw is unwrapped to take the short way between keys. A key without `fov=` takes the previous key's;
with none in the path, the game's FOV. The HUD is hidden while it plays. At the end, or on
`path.stop`, `view.leave` or `view.go`, the camera goes back to the game; free fly (F8) stops the
path and flies on from where it was.

**Benchmark.** `bench.run PATH [runs] [label]` plays the path in real time, after one warm-up pass,
the given number of times (default 3). It turns vsync off and `game.lockFps` to 0 for the
benchmark and restores both afterwards. It writes a per-frame CSV (frame, CPU frame time, GPU busy
time, per pass, draws) and a summary: average, 1 % low (the average of the slowest 1 % of frames),
worst frame, per-pass averages, the spread between runs, and whether the path is CPU or GPU bound.
GPU pass spans include idle time when the CPU is the bottleneck ([dev.md](dev.md)), so the
bound is decided per frame by comparing CPU frame time with GPU busy time (the sum of the GPU's
work, not the span from start to end of the frame); the summary gives the share of frames that were
GPU bound. Each labelled benchmark appends its summary to a per-map history file, so frame time and
draw counts can be charted across milestones.

As built (`ViewTool.cpp`; renderer side `RendDX9/ToolsBench.cpp`, `GpuTimers.cpp`): `bench.run PATH
[runs] [label]` (runs 1–20, default 3) plays the warm-up and the runs back to back, without handing
the camera back between them; `bench.stop` ends it without files, as does anything that stops the
path (`path.stop`, `view.go`, `view.leave`, free fly); `path.play` is refused meanwhile;
`bench.last` says which pass is drawing, or gives the last result line. `game.lockFps` refuses 0,
so the benchmark skips BF2Engine's client frame limiter instead and leaves `lockFps` as it was;
vsync goes through `renderer.vsync`, which resets the device when it changes (the warm-up absorbs
it). The renderer records each presented frame of a run: the time between Present returns (the
frame time), the time Present blocked, the draws and primitives of the frame's statistics row, and
the GPU time per pass, filed under its frame when the timestamp queries come back (the timers run
while the benchmark records, eight frames in flight so that uncapped frames are not dropped). Twelve
frames after the last run, still uncapped, the tool writes `<tools.outputDir>/<map>/bench/<PATH>
[-<label>]-<time>.csv` (one row per frame: run, frame, time, frame, CPU and Present-wait ms, GPU
ms, GPU bound, draws, primitives, one column per pass) and `.txt` (per run and overall: frames,
average, 1 % low, worst, CPU, wait and GPU means, draws, primitives, GPU-bound share; the spread
between runs, (largest − smallest) / mean, of the average and the 1 % low; per-pass means).

GPU busy time is not measurable with D3D9 timestamps: the pass spans include the GPU's idle time
when the CPU is the bottleneck, and under DXVK they sum to about the frame time either way. A
frame is therefore GPU bound when Present blocked for 10 % of its frame time or more (the driver
holds Present while the GPU is frames behind); CPU time is the frame time less that wait. Not yet
built: the history file and the acceptance check.

Accepted when: three runs of each of the Muttrah City and Saaremaa benchmark paths agree within
2 % on average frame time and 5 % on 1 % low (stated in the report, so render milestone 9's gains can
be told from noise).

### 4. Renderer inspection

Runs alongside render milestone 9; parts may be its first tasks.

- **Freeze culling.** `tools.freezeCull 1` keeps culling and LOD selection at the current camera
  while the camera moves on, to see what was submitted.
- **Draw breakdown** per view and in the overlay: draws and primitives for terrain, static meshes,
  bundled meshes, overgrowth, roads, decals and each shadow cascade, and the most expensive objects.
- **Debug shading modes** (`renderer.debugView`): albedo, normals, lighting only, cascade colours,
  LOD level colours, overdraw, wireframe, mip level. The shaders implement the modes.
- **Pass toggles:** skip a pass and see the frame and the timings change, live.
- **Pass markers** (`D3DPERF_BeginEvent`) named like the GPU spans, so captures read by pass.
- **Pixel inspector:** the colour, depth and linear depth under the cursor, the normal in debug
  modes, and a luminance histogram (for auto-exposure, render milestone 13).
- **Overrides:** time of day, sun angle, fog.
- **Variable sweep:** `view.sweep VAR FROM TO STEP` shoots the current view per value and writes
  one strip with the timings.

Accepted when: at a Muttrah City view, the draw breakdown's totals match the frame's draw count,
every debug mode and pass toggle works in the menu and from the console, a RenderDoc capture reads
by pass marker, and a variable sweep writes its strip.

### 5. Repeatability and map coverage

After render milestone 9; what milestones 1–3 left out.

**Bookmarks:** while free fly is on, Ctrl+F1..F8 store the current camera and F1..F8 go back to it,
for quick A/B looks without naming views. Not saved. Outside free fly, F1..F8 keep switching vehicle
seats ([ui.md](ui.md)); free fly takes the input from the soldier, so the keys never mean both.

**Fixed-step clock.** Besides real time for benchmarks (frame times as they are), a fixed step for images (frame N
looks the same in every run). The fixed step drives the frozen renderer time from milestone 1 as
well as the camera, so the image at frame N does not depend on how fast frames drew.
`path.play NAME [real|fixed]`, `path.stop`.

**Path editing** in the Views tab: add a keyframe at the current camera, preview the path and draw
it in the world while editing.

**Hitch log.** During paths and benchmarks (and in normal play with `tools.hitchLog 1`), frames over
`tools.hitchMs` (default 50 ms) are logged with what happened in them: an effect compiled at run
time, textures streamed in, effects created. It reuses the timed spans `BF2_LOAD_TRACE` already
records ([perf-load.md](perf-load.md)) rather than a second set of probes.

**Perf map.** `bench.map [step]` samples the map on a grid (default 64 m) at four headings, with
the camera 2 m above the terrain, unattended, and writes frame time and draw counts per cell as an
image over the minimap and as a table. Each sample settles and then averages 30 frames; a 2 km map
at the default grid is about 1,000 cells and 4,000 samples, roughly 20 minutes. Any cell can be
saved as a view, so the worst places on a map are found rather than guessed.

Accepted when: two shots of the same view are bit-identical, or the report names what cannot be
frozen; a fixed-step path gives the same frame N in two runs; the hitch log names the work in a
frame over `tools.hitchMs`; and a perf map of one map is produced.

### 6. Soldier mode and capture

- **Soldier mode:** the local server spawns a kit at a view's position and heading, so views can
  show the first-person model (toggled per view) and its shaders. Optionally a vehicle at the view
  (cockpits, vehicle shaders). How the server is told to spawn and place the soldier is
  investigated first.
- **Capture at a view:** `view.capture NAME` goes to the view, settles and triggers a RenderDoc
  capture through RenderDoc's in-process API.
- **Load stress:** spawn vehicles and soldiers at a view, or trigger explosions and smoke, for the
  cost of skinned meshes and particles.
- **Settings matrix:** run a set under several profiles (antialiasing, resolution, shadow quality).
- **Temporal metric:** on a slow fixed-step path (milestone 5), the difference between consecutive frames, so
  LOD popping, shadow shimmer and crossfade flicker become a number.

Accepted when: a view with soldier mode on shows the first-person model and a vehicle view shows
its cockpit, both deterministic under the milestone 1 check; `view.capture` writes a capture
unattended; and a settings matrix run produces one report per profile.

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

- `[settings]`: the `renderer.exposure*`, `bloom*` and `encodeSrgb` values, the adaptation rates
  and the key. `[frame]`: frames run, whether the last ran (or thermals skipped it), its dt and
  whether it snapped, the scene's size.
- `[state]`: adapted and metered EV (and luminance), the exposure as a multiplier and in stops.
- `[histogram]`: each non-empty bin's low EV, weight, share and cumulative share.
- `[meter]`: the 64 x 32 log2 luminances' range and mean, and a sparse grid of them.
- `[bloom]`: level 0 after the up chain, statistics and `<NAME>-bloom.png`.

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

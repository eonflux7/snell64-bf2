# Load times

The `perf` lane ([AGENTS.md](../../AGENTS.md)): make loading faster without changing what is
loaded. The target is the **first map load**, menu to spawn screen, with PR. Frame time in the
running game is in [perf-frame.md](perf-frame.md).

## Measuring

`BF2_LOAD_TRACE=<prefix>` ([dev.md](dev.md)) records timed spans on the main thread (and later
on worker threads). Each image (BF2.exe, RendDX9, BF2Audio) writes its own file, and all share one clock:

| Category | Site | What it covers |
|---|---|---|
| `phase` | `BF2Engine::init`, `initEngine`, `startGame`, `GameLogic::load` | `boot`, `engine.init`, `game.start`, `level.load` |
| `mark` | `GameLogic::load`, `LoadStat::setNextLoadPart`, first `renderMenu`, autohost | `load:*` sections (`load:resources` when the loading bar ends), `part:*` loading-bar phases, `menu.first`, `menu.host` (where a player's click would be) |
| `start` / `setup` | `BF2Engine::startGame`, `GameLogic::load` | Hosting: `game.init`, `gameLogic.init`, `startServer`; level setup: `beginLoadStats`, `createSunTemplate`, `loadingTemplatesEvent` |
| `init` | `BF2Engine::init`, `initEngine` | Startup stages: `modules` (inside it `modules.construct`, `modules.register`, `modules.singletons` and one span per module init, named by class; `init.register` splits the registration per module into `getClassList`, `classes` and one span per singleton registered; `tsc.calibrate` (worker) and `tsc.join`; `video.detect` when the heuristic runs), `filesystem`, `archives`, `localization`, `usersettings`, `modinfo`, `renderer` (inside it `renderer.window`, `renderer.dll`, `renderer.initialize`), `input`, `hud`, `swiff`, `network` |
| `mount` | `FileManager::mountArchive` | Opening an archive and reading its directory |
| `open.arc` / `open.loose` / `open.miss` | `FileManager::open` (read) | Where the file resolved; archive entries up to 64 KiB are inflated here |
| `exists.arc` / `exists.loose` / `exists.miss` | `FileManager::fileExistsEx` | The same for existence probes |
| `con` | `OldConsole::run` | One script, with nested scripts as children; inside it `con.read` (the file read) and `con.cmd` (one per invoked binding, named `Object.method`). The `con` self time is parsing and dispatch; each trace mark also prints `[CONPROFILE]` to stderr, the interpreter's time per stage since the previous mark (`split`, `compiler`, `firstToken`, `prep`, `keyword`, `exchange`, `lookup`, `args`, `checks`, `post`) |
| `tex` | `TextureManager::loadTextureFromFile` | One texture; inside it `tex.find` (path lookup), `tex.open` (open and header), `tex.create` (the D3D texture), `tex.read` (one per mip read into the locked level, inflate included) |
| `mesh` | `MeshTemplate::loadFromStream` | Static, bundled and skinned mesh files; inside it `mesh.read` (with `mesh.geometry`, the vertex and index blocks), `mesh.merge`, `mesh.buffers` (vertex and index buffers), `mesh.finalize`. `mesh.instances` is the template's walk over its live instances after the load |
| `snd.decode` / `snd.upload` | `SoundFile::load` (BF2Audio) | Reading and decoding one sound file; its OpenAL upload. `snd.unload` and `snd.sort` are the sweeps around the loads |
| `fx.src` | `compileEffectFile` (`RendDX9/Effect.cpp`) | Effects compiled from `.fx` source; inside it `fx.src.cache` (the bytecode cache lookup) and `fx.src.compile` |
| `fx.dice` | `DiceEffect::compile` | Dice effects; `fx.compile` inside it when its cache missed |
| `file.prefetch` / `file.wait` | `ArchivePrefetch.cpp` | One entry inflated by a prefetch worker; the main thread waiting for one in progress. `file.prefetch.queue` reads the level's list, `file.prefetch.end` writes it |
| `fx.ra` | `EffectManager::compileEffect` | Ra effects; inside it `fx.read` (the source), `fx.stat` (each file-info lookup), `fx.key` (the content key), `fx.keyfile` (the stored key), `fx.setup` (before the source read), `fx.listen`, `fx.event` (the loading-progress event), `fx.cacheload`, `fx.compile` (one per shader profile attempted), `fx.compile.fail`, `fx.create` |
| `py.init` / `py.admin` / `py.gamemode` / `py.event` | `PythonHost` | Interpreter start, admin module, the game-mode import and init, event handlers of 1 ms or more |
| `loadscreen` | `BetweenRoundsLogic::init` | Building the loading screen: `initSwiff`, `MenuTeamManager.init`, `controlSettings.apply`, `SwiffRender.init`, `loadMovie` |
| `swiffimg.create` / `swiffimg.load` | `registryGetOrLoad` (`Geom/SwiffRender.cpp`) | A menu image created by D3DX; one read from the image cache |
| `probe.list` / `probe.drop` | `Win32FileSystem.cpp` | A directory listed for existence and file-information queries (bytes: its entries); a name written, deleted, moved or copied while the listings are active |
| `event` | `EventManager::dispatchEvent` | One delivery to a listener of 1 ms or more, named by the listener's class, category and id |
| `frame` | `BF2Engine::mainLoop` | Frames over 50 ms; after a load, these show work deferred to first use |

`Code/Tools/LoadTrace/loadtrace.py <prefix>` nests the spans and prints, per phase, the self time
of each category, the time between marks, and the heaviest names. "(untraced)" is phase time
that no instrumented span covers.

## Baseline

First load of muttrah_city_2 (PR, autohost, Wine), effect cache not invalidated, before any
`perf` change. From the menu click to the end of loading: about 190 s.

| Where | Time | What |
|---|---|---|
| Ra effect compiles | 176 s | 1338 effects, all compiled from source: the `.cfx` cache was never used under Wine (below). About 37 s are terrain effects during `Init.con`/`Sky.con` (0.6 to 0.76 s each), 133 s are mesh effects in the loading bar's Geometries part |
| Console scripts | 9 s | 13,305 runs |
| File opens | 1.5 s | 40,000 opens, mostly from archives |
| Startup to menu | 10.4 s | 5.2 s HUD scripts, 2 s `compileEffectFile` (`fsquaddrawer.fx` alone 1 s) |

The Ra cache key is computed with `D3DXPreprocessShader` from `d3dx9_24.dll`, which does not
export it (neither Microsoft's x64 build the game loads nor Wine's), so every key was empty and
every `.cfx.key` held `-`.
The key now falls back to `d3dcompiler_47`'s `D3DPreprocess`. With the cache filled, the same load
takes 37 s instead of 186 s (to the loading bar's Textures part): Ra effects 19.5 s, of which 16.6 s
is computing the content key (about 12 ms per effect) and 0.5 s reading the 1337 blobs. Console
scripts (9.4 s, all in the Objects part) are now the largest item.

### Recompiling every effect

With every Ra effect stale (after a shader edit, or a new cache), the same load reached the
Textures part in 186 s with the serial compile and in 31 s with the prefetch. 8 workers, each with
its own copy of the compiler module, ran 202 s of compiles in 25 s of wall time, mostly before the
main thread asked for the effects.

8 compiles of a test effect took 0.75 s serially, 0.77 s on 8 threads sharing `d3dx9_24.dll`, and
0.16 s on 8 threads with a renamed copy each.

## Constraints

- The D3D9 device is created without `D3DCREATE_MULTITHREADED`. Worker threads may compile
  bytecode and read or inflate files; every device call stays on the main thread.
- Console scripts, template creation and Python run in order and change global state. They stay
  serial; only the file work under them can move to other threads.
- Effects compile with `d3dcompiler_47`, which runs compiles side by side in one module (render
  milestone 15). Under `BF2_FX_COMPILER=d3dx`, D3DX's compiler runs one compile at a time per loaded
  module, so each worker gets a renamed copy of it.

## State

muttrah_city_2 skirmish, autohost, warm caches: boot to the first menu frame 1.4 s (10.4 s at the
baseline); the menu click to the end of the loading bar 14.2 s (about 190 s at the baseline, 22.5 s
with the caches and prefetch off). Every change below was accepted in game and its flag retired
(2026-11-17); the commit messages carry the full measurements.

| Change | Lane | Measured |
|---|---|---|
| Load trace, `BF2_LOAD_TRACE` | `dev` | — |
| Ra cache key falls back to `D3DPreprocess` under Wine | `fix` | Filled cache: 186 s to 37 s |
| Ra cache key from source and includes, no preprocessor | `perf` | Keys 16.6 s to 2.2 s per load |
| Ra effect prefetch: stale effects in `effects.lst` compile on 8 workers | `perf` | Full recompile 186 s to 31 s |
| Effects compile with `d3dcompiler_47`, a worker per core but one (render milestone 15) | `render` | test_bootcamp, every effect stale: about 10 min to 30 s |
| `effects.lst` appended to, not replaced (it held one name); a new cache directory seeds it from the newest sibling's | `perf` | Not yet measured |
| Each include file's info looked up once per frame (`FXSTAT`) | `perf` | −4.2 s (menu switch and Geometries) |
| Bytecode cache for `compileEffectFile`, `@HOME@/<mod>/cache/fxsrc/` (`FXSRC`) | `perf` | Boot to menu 10.6 s to 8.75 s |
| Console lines tokenized without `std::istringstream` (`CONPARSE`) | `perf` | About −4.5 s of part:Objects |
| Console dispatch: dotted first token skips keyword compares; binding range cached (`CONDISPATCH`) | `perf` | part:Objects 9.0 s to 7.9 s |
| Id and name index for ClassManager's class list (`CLASSINDEX`) | `perf` | About −50 ms |
| TSC calibration on a worker, joined at the end of engine init (`TSCCAL`) | `perf` | −1.0 s of startup |
| Video quality heuristic skipped where its result is dropped (`VIDEODETECT`) | `perf` | Renderer creation 1.48 s to 0.45 s, plus 1 s per profile login |
| Name index for the HUD's BFMeme registry (`MEMEINDEX`) | `perf` | HUD setup was 5.3 s, quadratic |
| Loading-screen repaints from effect loads capped at 30 per second (`FXEVENT`) | `perf` | −1.15 s in Geometries |
| Loose-file probes answered from cached directory listings during a game start and level load (`PROBECACHE`) | `perf` | Load 15.5 s to 13.8 s; misses 936 ms to 186 ms |
| Menu image cache, `@HOME@/<mod>/cache/swiffimg/` (`SWIFFIMG`) | `perf` | Level setup 0.76 s to 0.09 s |
| Archive prefetch: next load of a level inflates its listed entries on 4 workers, ≤256 MiB ahead (`FILEPREFETCH`) | `perf` | Load 21.6 s to 15.7 s |

Special names (wildcards, short names, alternate streams, non-ASCII, case-only differences) still
ask the disk under the probe cache, and a name written, deleted, moved or copied asks the disk for
the rest of the window.

Not done, not scheduled: a shared job system, and decoding textures and meshes on workers (the D3D
resources would still be created on the main thread).

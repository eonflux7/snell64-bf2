# openbf2-eon64

An x64 Battlefield 2 client that runs the Project Reality mod (PR) offline and replaces BF2's
renderer with a modern pipeline. It started as a fork of [OpenBF2](https://github.com/kr4uzi/OpenBF2)
and is now developed on its own.

This is a work in progress. A successful build is not a playable game; where each area stands is in
[docs/STATUS.md](docs/STATUS.md).

## What it does

- **Runs PR 1.9 on x64.** PR's closed hook DLL (`rhooks.dll`) and its 32-bit Python extension cannot
  load into a 64-bit engine, so the engine provides what PR needs from them instead: CPython 2.7.12
  as `dice_py.dll`, PR's built-in Python modules, HUD variables, the launcher handshake. See
  [compat.md](docs/lanes/compat.md).
- **Replaces the renderer.** BF2's forward, gamma-space, lightmap-lit renderer becomes a linear HDR
  pipeline. Done: reversed-Z, a depth prepass, cascaded sun shadows, linear particle lighting, 4x
  MSAA with LOD crossfades, draw submission without D3DX's per-draw overhead (instanced),
  single-pass terrain. In progress: a physical sky and aerial perspective with a radial view
  distance. Planned: sky reflections, clouds, bloom with auto-exposure, GTAO. See
  [docs/render/](docs/render/README.md).
- **Loads faster.** A PR map load, menu to spawn screen, went from about 190 s to 14 s (effect
  caches, worker compiles, file prefetch). See [perf-load.md](docs/lanes/perf-load.md).
- **Has in-game tools.** An ImGui debug menu, performance overlay and console
  ([ui.md](docs/lanes/ui.md)); console-driven views, screenshots, camera paths and benchmarks
  ([tools.md](docs/lanes/tools.md)).
- **Fixes engine bugs** found along the way (crashes, x64 hazards), as standalone commits that can
  be offered to OpenBF2. See [fix.md](docs/lanes/fix.md).

Work is split into lanes (`fix`, `compat`, `render`, `perf`, `build`, `dev`, `ui`, `tools`), each
with its own rules; see [AGENTS.md](AGENTS.md).

## Building

Windows x64 only: MSVC (C++14), Ninja and CPython 2.7 headers (`BF2_PYTHON27_SRC`). x86 is not
built.

- `BF2.exe` and the four plugin DLLs (RendDX9, TextureAtlasBuilder, SwiffPlayer, BF2Audio) are
  incremental CMake targets: a one-file renderer change rebuilds in about 8 s.
- Plugin sources are listed once, in `Code/Build/<plugin>_sources.txt`, and read by both CMake and
  the `.cmd` scripts.
- Each plugin link runs an ABI check (`Code/Build/plugin_abicheck.cmake`) that deletes the DLL if
  its exports, imports or CRT drifted. `bf2_client_plugins` stages the DLLs next to `BF2.exe`.
- The plugin targets compile with `/O2` (`BF2_PLUGIN_OPTIMIZE`, default on); the `.cmd` scripts
  build without optimisation. RendDX9's seam build (`BF2_GFX_SEAM`) still uses `renddx9.cmd`.

OpenBF2's toolchain notes, [docs/archive/building.md](docs/archive/building.md) and
[docs/archive/getting-started.md](docs/archive/getting-started.md), still describe the toolchain itself.

## Running

The client runs against an existing PR install through an overlay; the install itself is never
modified. Under Proton/DXVK:

- set `DXVK_CONFIG="d3d9.useD32forD24 = True"` (reversed-Z needs float depth);
- place Microsoft's x64 `d3dx9_43.dll` and `d3dcompiler_43.dll` (DirectX June 2010 runtime) and
  `d3dcompiler_47.dll` (Windows SDK redistributable) beside `BF2.exe` and load them native
  (`WINEDLLOVERRIDES=d3dx9_43,d3dcompiler_43,d3dcompiler_47=n`); Wine's builtins cannot compile
  effects.

Play is offline and client-hosted. `BF2_AUTOHOST=<map>:<mode>:<size>` hosts straight from the menu.
Every flag this project adds is listed in [dev.md](docs/lanes/dev.md).

## What is not here

PR's shaders, scripts and Python shims are closed content and are not part of this repository. The
documents here describe only the engine side of each contract (console commands, event ids, sampler
slots, shader constants).

## Origin

```
B2BF/OpenBF2 ─► kr4uzi/OpenBF2 ─► + realitymod/OpenBF2 PR #1 ─► openbf2-eon64
                                    (PR game-logic port)
```

OpenBF2 reconstructs the Battlefield 2 1.50 engine from the shipped binaries; this project would not
exist without it. It is no longer merged in. Its documentation remains under [docs/archive/](docs/archive/README.md)
as reference; this project's own is in [docs/](docs/) ([STATUS.md](docs/STATUS.md), [PLAN.md](docs/PLAN.md), [render/](docs/render/README.md), [milestones/](docs/milestones/), [lanes/](docs/lanes/)).

## Contributing

Read [AGENTS.md](AGENTS.md) first: lanes, milestones, commit format, source markers, flags and what
"done" means. Agents do not open pull requests or answer reviewers; the human author does and is
responsible for the change.

## License

See [LICENSE](LICENSE).

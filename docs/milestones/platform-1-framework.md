# Platform milestone 1: framework and 2D

Tasks and checks of an open milestone; its order and state are in [PLAN.md](../PLAN.md#platform),
its design and rules in [lanes/platform.md](../lanes/platform.md). On acceptance its facts are folded
into the lane document and this file is deleted.

Done when: main menu to in game on Muttrah City, the world empty, the HUD, menus, console and ImGui
drawn by `render`.

Tasks:

0. **Baseline.** On `main`: a shot of every saved view at 1920x1080 4x and the benchmark paths'
   numbers, kept with the build they came from; the script that shoots every view and puts each
   beside `main`'s.
1. **Branch and module.** `platform` from `main`; `namespace render` and where it lives in RendDX9;
   `Shaders/` and how the game finds it (the build stages it, the dev overlay links it).
2. **Loader.** Every `.hlsl` entry point compiled with `d3dcompiler_47` on workers, shaders created
   on the main thread, recompiled on reload; compile errors to the log `rdev.sh` summarises. Timed:
   startup and reload.
3. **Layout.** `shader_layout.h` (constant blocks, sampler table) served through the compiler's
   include handler; block upload when changed; sampler states from the table.
4. **Pipeline states.** The named states and the cached device state.
5. **Frame.** The frame function, the named render targets (re-created on reset), GPU pass timers;
   the old frame no longer called.
6. **Register report.** Temporaries and instruction slots per shader from `D3DDisassemble`, against
   each pass's budget.
7. **Draw-list boundary.** The engine hands `render` its draw items and lights each frame (nothing
   draws them yet).
8. **2D.** The batcher; Swiff's drawing, fonts (and whether HUD text uses the Swiff font wrapper),
   the HUD, nametags' 2D parts, the loading screen, the console, ImGui onto it. Geom and GeomMisc
   stop calling D3D9 for these.
9. **Delete.** Old renderer code nothing calls any more on the branch.
10. **Checks.** Main menu, server browser, loading screen, in game on Muttrah City with the HUD;
    `rdev.sh run`, `cmd`, `shot`, `view.shot` working.

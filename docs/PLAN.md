# Plan

What each lane does next: its milestones, their state and their order. An open milestone's design
and tasks are in its own document under [milestones/](milestones/) (`render`) or in its lane's
document under [lanes/](lanes/) (`tools`, `ui` 1–4; `ui` 5–11 share [milestones/ui-menu.md](milestones/ui-menu.md)); what is active now is in [STATUS.md](STATUS.md).
Milestones go by ID (`render-23`; [GLOSSARY.md](GLOSSARY.md#ids)). An open milestone's state cell
is generated from its document (`tools/docs.py gen`); the others are edited by hand, and `check`
compares them with their documents. When the owner accepts a milestone, its row says so, with its
commits (`tools/docs.py log <id>`) and, once its document is deleted, where to read it:
`git show <commit>^:docs/milestones/<file>`.

## `render`

Open milestones: [milestones/](milestones/); accepted ones are folded into the renderer as built,
[render/](render/README.md).

| Milestone | State | Engine commits |
|---|---|---|
| `render-1` Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| `render-2` Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| `render-3` Shared lighting model | Accepted (shader-only) | — |
| `render-4` Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| `render-5` Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| `render-6` Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| `render-7` Particle lighting | Accepted | `b1549478`, `25319adf`, `7fcdc848`, `423ac5e0`, `0bffa203`; fix `6eb1a727` |
| `render-8` Anti-aliasing | Accepted | `28487a35`, `65760125`, `f11bdfe3`, `b47ac87f`, `0f10eb41`, `f9086318`, `ee0e7ed8`, `98165f0b`, `b3e44a8d`, `a7d9e0e1`, `1e7a1c89`, `680d8890` |
| `render-9` Draw submission | Accepted | `2eb902ce`, `0a623df8`, `e87543df`, `f3515fb4`, `7bdd3a77`, `4bc56ff8`, `e9b6a8a4`, `965bb8bf`, `c2f7ef33`, `3c745649`, `2002788d` |
| `render-10` Terrain in one pass | Accepted | `2eb4d02d`, `c21a012e`, `0d9e15a6`, `57fa2899`, `3b9da48c`, `6fd523bb`, and the deletion ("milestone 10 accepted") |
| `render-11` Atmosphere, sky IBL, water reflections | Accepted | `tools/docs.py log render-11`, and the deletion ("milestone 11's switches and the sky dome deleted") |
| `render-15` Shader pipeline (ahead of 12) | Accepted | `a14e57a6`, `cbbe8725` |
| `render-12` Clouds | Paused: tasks 6 seen, 1–5, 7–8 built, 10 planned, 9 proposed | `tools/docs.py log render-12` |
| `render-16` Water | Accepted | `tools/docs.py log render-16`, and the deletion ("milestone 16 accepted") |
| `render-13` HDR bloom + auto-exposure | Accepted (owner, 2026-11-23) | `tools/docs.py log render-13` |
| `render-14` Run-time sun term (lightmaps' sun channel dropped) | Accepted (owner, 2026-11-23) | `dc1a7223`–`c1060597` |
| `render-17` Far terrain | Paused: tasks 1–5 seen, 6–7 planned | `tools/docs.py log render-17` |
| `render-18` Atmosphere rework (one atmosphere, weather per level) | Accepted (owner, 2026-11-23) | `tools/docs.py log render-18` |
| `render-19` Hex-tiled detail textures | Accepted | `tools/docs.py log render-19` |
| `render-20` GTAO (split out of `render-14`: small-scale occlusion) | Proposed; not planned (D-26) | — |
| `render-21` Tonemap and grade | In progress: tasks 1–2, 4, 6 built, 5 planned, 3 deferred | `tools/docs.py log render-21` |
| `render-22` Far overgrowth (the whole map's trees past the level's overgrowth range) | In progress: tasks 1 built, 3–5 planned, 2 deferred | `tools/docs.py log render-22` |
| `render-23` Water at the shore, water types, caustics, rivers, whitecaps | In progress: tasks 1–7 built, 8–9 planned | `tools/docs.py log render-23` |

The owner accepts each milestone in game, by eye, against the `tools` lane's reference views
([tools.md](lanes/tools.md)). GPU cost waits for one pass once the planned milestones are in (D-20): the GPU pass timers ([render/README.md](render/README.md#gpu-pass-timers)) and the benchmark
paths, each milestone's cost stated against milestone 11's baseline; a regression stands only if
the owner accepts it explicitly, and is recorded with the milestone in its subsystem's document under [render/](render/README.md).

Bloom and auto-exposure depend only on the HDR scene and can move earlier if night layers need
them sooner.

Not planned: converting the assets to a PBR material set (the existing maps are interpreted as PBR
inputs in milestone 11). Deferred until milestones 8–13 are done (D-1): a Vulkan
backend or DXVK interop for per-sample MSAA reads and custom resolves.

## `tools`

Design: [lanes/tools.md](lanes/tools.md#milestones).

| Milestone | Commit scope | Status |
|---|---|---|
| `tools-1` Views | `tools(view)` | Paused: tasks 1–2 seen, 3–5 built, 6–8 planned |
| `tools-2` Unattended runs and regression | `tools(run)` | Planned |
| `tools-3` Benchmark path | `tools(bench)` | Built; cut: no history file or acceptance check |
| `tools-4` Renderer inspection | `tools(inspect)` | Planned; `tools.survey` and `tools.atmosphere` built ahead of it |
| `tools-5` Repeatability and map coverage | `tools(bench)` | Planned |
| `tools-6` Soldier mode and capture | `tools(soldier)` | Planned |

The lane is paused (D-2): milestones 1 and 3 were cut to what render milestone 9
needed. The rest is picked up when a render task needs it. Bookmarks, bit-identical shots, the
fixed-step clock, path editing in the Views tab, the hitch log and the perf map are milestone 5.

## `ui`

Design: [lanes/ui.md](lanes/ui.md#milestones); milestones 5–11 (the front-end menu on RmlUi):
[milestones/ui-menu.md](milestones/ui-menu.md).

| Milestone | Commit scope | Status |
|---|---|---|
| `ui-1` ImGui layer | `ui(imgui)` | Accepted (`71545137`, `f4a7fd65`, `6604b184`) |
| `ui-2` Sun shadow tuning as console variables (lane `render`) | `render(csm)` | Accepted (`2e605f93`) |
| `ui-3` Debug menu and performance overlay | `ui(debug)` | Accepted (`ae0ee355`, `d9086de0`, `4b10dea9`; tabs and console polish `134889d5`) |
| `ui-4` Console frontend | `ui(console)` | Accepted (`cfc56237`); retail drawing deleted |
| `ui-5` RmlUi foundations: libraries, scripting, draw device, menu driver | `ui(menu)` | In progress: tasks 1 built, 2–4 planned |
| `ui-6` Menu service boundary (`MenuHost`, `Menu/Api`) | `ui(menu)` | Proposed |
| `ui-7` Main menu and options on RmlUi | `ui(menu)` | Proposed |
| `ui-8` Multiplayer: server browser, join, local and co-op games, demos | `ui(menu)` | Proposed |
| `ui-9` Between rounds: loading, end of round, Escape menu | `ui(menu)` | Proposed |
| `ui-10` The mod's menu on the new stack (private content) | — | Proposed |
| `ui-11` Flash removed | `ui(menu)` | Proposed |

## `perf`, `compat`, `fix`

No milestones. What is planned or missing is in each lane's document:
[perf-load.md](lanes/perf-load.md#state), [perf-frame.md](lanes/perf-frame.md#state),
[compat.md](lanes/compat.md#missing), [fix.md](lanes/fix.md).

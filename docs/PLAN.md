# Plan

What each lane does next: its milestones, their state and their order. A milestone's design is in
its lane's document under [lanes/](lanes/); the current milestone's tasks are in
[STATUS.md](STATUS.md). When the owner accepts a milestone, its row here says so, with its commits.

## `render`

Design: [lanes/render.md](lanes/render.md#milestones).

| Milestone | State | Engine commits |
|---|---|---|
| 1. Reversed-Z | Accepted | `cb37584f`, `d7481b9c` |
| 2. Linear lighting + HDR scene | Accepted | `edc83e55`, `5eb26d06`, `2ee49ec7`, `1c2257b8` |
| 3. Shared lighting model | Accepted (shader-only) | — |
| 4. Depth prepass + screen-space sun mask | Accepted | `3a00bf99`, `988774bf`, `82e04f61` |
| 5. Sun cascaded shadow maps + PCF | Accepted | `271c8d45`, `df9a6a40`, `6261ac82`, `5ccaf770`, `505ca7ab`, `2860a74a` |
| 6. Shader audit and cleanup | Accepted | `ee33008f`, `7f05cc77`, `88d3b7f1`, `52af0169`, `f6fb255a`, `e7623d07`, `dc97ef7b` |
| 7. Particle lighting | Accepted | `b1549478`, `25319adf`, `7fcdc848`, `423ac5e0`, `0bffa203`; fix `6eb1a727` |
| 8. Anti-aliasing | Accepted | `28487a35`, `65760125`, `f11bdfe3`, `b47ac87f`, `0f10eb41`, `f9086318`, `ee0e7ed8`, `98165f0b`, `b3e44a8d`, `a7d9e0e1`, `1e7a1c89`, `680d8890` |
| 9. Draw submission | Accepted | `2eb902ce`, `0a623df8`, `e87543df`, `f3515fb4`, `7bdd3a77`, `4bc56ff8`, `e9b6a8a4`, `965bb8bf`, `c2f7ef33`, `3c745649`, `2002788d` |
| 10. Terrain in one pass | Accepted | `2eb4d02d`, `c21a012e`, `0d9e15a6`, `57fa2899`, `3b9da48c`, `6fd523bb`, and the deletion ("milestone 10 accepted") |
| 11. Atmosphere, sky IBL, water reflections | Accepted | `git log --grep 'milestone 11'`, and the deletion ("milestone 11's switches and the sky dome deleted") |
| 15. Shader pipeline (ahead of 12) | Accepted | `a14e57a6`, `cbbe8725` |
| 12. Clouds | Paused (owner, 2026-11-22) after tasks 1–5, 7–8; open tasks in STATUS.md | `git log --grep 'milestone 12'` |
| 16. Water | Accepted | `git log --grep 'milestone 16'`, and the deletion ("milestone 16 accepted") |
| 13. HDR bloom + auto-exposure | Accepted (owner, 2026-11-23) | `git log --grep 'milestone 13'` |
| 14. Run-time sun term (lightmaps' sun channel dropped) | Built, awaiting the owner's look | `dc1a7223`–`c1060597` |
| 17. Far terrain | In progress: tasks 1–5 built, seen by the owner (fine for now); bakes for more levels next | `git log --grep 'milestone 17'` |
| 18. Atmosphere rework (one atmosphere, weather per level) | Accepted (owner, 2026-11-23) | `git log --grep 'milestone 18'` |
| 19. Hex-tiled detail textures | Accepted | `git log --grep 'milestone 19'` |
| 20. GTAO (split out of 14) | Not planned | — |
| 21. Tonemap and grade | In progress: tasks 1–2 built | `git log --grep 'milestone 21'` |

The owner accepts each milestone in game, by eye, against the `tools` lane's reference views
([tools.md](lanes/tools.md)). GPU cost waits for one pass once the planned milestones are in (owner,
2026-11-22): the GPU pass timers ([render.md](lanes/render.md#gpu-pass-timers)) and the benchmark
paths, each milestone's cost stated against milestone 11's baseline; a regression stands only if
the owner accepts it explicitly, and is recorded with the milestone in [render.md](lanes/render.md).

Bloom and auto-exposure depend only on the HDR scene and can move earlier if night layers need
them sooner.

Not planned: converting the assets to a PBR material set (the existing maps are interpreted as PBR
inputs in milestone 11). Deferred until milestones 8–13 are done (owner, 2026-11-18): a Vulkan
backend or DXVK interop for per-sample MSAA reads and custom resolves.

## `tools`

Design: [lanes/tools.md](lanes/tools.md#milestones).

| Milestone | Commit scope | Status |
|---|---|---|
| 1. Views | `tools(view)` | Tasks 1–5 built; 6–8 (settle, stamping, determinism) planned |
| 2. Unattended runs and regression | `tools(run)` | Planned |
| 3. Benchmark path | `tools(bench)` | Built, cut: no history file or acceptance check |
| 4. Renderer inspection | `tools(inspect)` | Planned; `tools.survey` and `tools.atmosphere` built ahead of it |
| 5. Repeatability and map coverage | `tools(bench)` | Planned |
| 6. Soldier mode and capture | `tools(soldier)` | Planned |

The lane is paused (owner, 2026-11-19): milestones 1 and 3 were cut to what render milestone 9
needed. The rest is picked up when a render task needs it. Bookmarks, bit-identical shots, the
fixed-step clock, path editing in the Views tab, the hitch log and the perf map are milestone 5.

## `ui`

Design: [lanes/ui.md](lanes/ui.md#milestones).

| Milestone | Commit scope | Status |
|---|---|---|
| 1. ImGui layer | `ui(imgui)` | Accepted (`71545137`, `f4a7fd65`, `6604b184`) |
| 2. Sun shadow tuning as console variables (lane `render`) | `render(csm)` | Accepted (`2e605f93`) |
| 3. Debug menu and performance overlay | `ui(debug)` | Accepted (`ae0ee355`, `d9086de0`, `4b10dea9`; tabs and console polish `134889d5`) |
| 4. Console frontend | `ui(console)` | Accepted (`cfc56237`); retail drawing deleted |

## `perf`, `compat`, `fix`

No milestones. What is planned or missing is in each lane's document:
[perf-load.md](lanes/perf-load.md#state), [perf-frame.md](lanes/perf-frame.md#state),
[compat.md](lanes/compat.md#missing), [fix.md](lanes/fix.md).

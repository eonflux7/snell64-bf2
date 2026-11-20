# Agent instructions: openbf2-eon64

The policy for every coding agent working on this repository; `CLAUDE.md` imports it.
If guidance conflicts, the owner's current request wins, then this file, then `docs/eon64/`.

An x64 Battlefield 2 client that runs the Project Reality mod (PR) offline and replaces BF2's
renderer with a modern pipeline. It began as a fork of OpenBF2 (kr4uzi/OpenBF2, downstream of
B2BF/OpenBF2) and is now its own project: OpenBF2's policy and documents do not govern it, and
OpenBF2 is no longer merged in. One tie remains: engine fixes stay separable, so they can be offered
upstream ([Upstreaming](#upstreaming)).

## Lanes

Every change belongs to exactly one lane.

| Lane | What it is | Rule |
|---|---|---|
| `fix` | A bug in the engine (crash, wrong behaviour, x64 hazard) | Smallest correct change. Match retail where the evidence exists; say so when it was not checked. Committed on its own and independent of other lanes' code |
| `compat` | What PR's content, scripts or launcher need from the engine | Specified by what PR does, never by guesswork about rhooks internals. No visual changes |
| `render` | The renderer: a new design, not retail | Clean rewrites, no retail constraint. No legacy path kept beside its replacement once the milestone is accepted |
| `perf` | Load and frame time: threading, caching, avoiding repeated work, cheaper algorithms | Changes what work costs, never what it produces: the loaded state matches the old path's, and frames draw the same |
| `build` | Toolchain, CMake/Ninja, build scripts, ABI checks | Never changes runtime behaviour |
| `dev` | Diagnostics and developer hooks (traces, autohost, file monitor) | Off by default; when unset, behaviour and cost are unchanged |
| `ui` | In-game UI on Dear ImGui: debug menu, performance overlay, console | Replaces the retail UI it supersedes. Hidden UI costs nothing. Never owns a setting: widgets change console variables. PR-specific content is read from files outside this tree |
| `tools` | The in-game test harness: views, camera paths, unattended runs, benchmarks, renderer inspection | Console first: every action is a console command or variable, results are files. Idle costs nothing. View files and debug shading modes live outside this tree |

A change that seems to need two lanes is two commits. A `fix` found during other work (for example
the StateManager MRT slot overflow, found in `render` work) is committed on its own, first.

## Milestones

`render`, `ui` and `tools` plan their work as numbered **milestones** in the lane's document. The
current milestone is broken into **tasks** in `docs/eon64/status.md`. Outside the lane's own
document, name the lane: "render milestone 9", "tools milestone 1". When the owner accepts a
milestone, its flag and any replaced path are deleted, its description moves from the plan to the
as-built document, and `status.md` moves on to the next. Commit messages before 2026-11-19 say
"step" for milestone.

## Repositories and what is public

- This repository (`eonflux7/openbf2-eon64`, branch `main`) is **public**. Only engine code and
  engine-side documentation go here.
- PR's shaders, decompiled PR scripts, PR's private Python shims and anything else derived from PR's
  closed content live in **private** repositories and directories outside this tree. Never copy them
  here, and never quote them in commits, comments or docs. Describing an engine-side contract that
  PR content relies on (a console command, an event id, a sampler slot) is fine.
- The machine-specific workspace (game installs, the PR overlay, the build VM, launch scripts) is
  documented outside this repository. Do not add personal paths or account details to this tree.

## Hard rules

- Never modify the original game installations; work only through the overlay.
- Stay offline: no online PR login, master server or launcher authentication.
- **Port 16567 is reserved** for the owner. Any agent-run server uses another explicit port.
- Do not create or submit pull requests, and do not reply to reviewers. Commit and push the branch
  only when asked or when it is part of an agreed workflow.
- Launch or restart the game when a test needs it, and say what was launched. When asked to check
  or screenshot, do it at once.

## Building and testing

- Windows x64 only, built in the build VM with MSVC (C++14). x86 is not built; do not claim x86
  results.
- `BF2.exe` and the four plugin DLLs are CMake/Ninja targets. Each plugin's sources are listed once,
  in `Code/Build/<plugin>_sources.txt`, which both CMake and the `.cmd` scripts read; keep them in
  step. A plugin that fails its post-link ABI check (`Code/Build/plugin_abicheck.cmake`) is deleted.
- Every compile line that touches `Code/BF2` states its image: `/DDEDICATED=0` for the client and
  its DLLs, `/DDEDICATED=1` for the server, always with `/FI Code/BF2/Core/BuildConfig.h`.
- **Done means seen in game.** A build or ABI check proves only that the image links. A `render`,
  `compat`, `ui` or `tools` change is done when the owner has seen it work in game. A `perf` load
  change is done when a traced load reaches the spawn screen with the same effects, meshes and
  textures as the old path. Say exactly what was exercised: built, ABI-checked, loaded, in game,
  which map, which flags.

## Commits

```
<lane>(<area>): <what changed>

<why; what was tested and how; the flag it sits behind, if any>

Co-Authored-By: ...
```

Examples: `render(csm): blend between cascades`, `perf(effects): compile on worker threads`,
`fix(ai): guard getIsland(-1) on x64`, `compat(console): SquadManager.listSquads rows`,
`build(plugins): Ninja targets`, `dev(console): BF2_CONSOLE_TRACE`, `ui(debug): frame-time graph`,
`tools(view): free fly`. Changes that belong to no lane (documentation, this file, repository-wide
renames) use `docs:` with no area.

## Source markers

Inherited code carries OpenBF2's provenance tags (`[CHOSEN, not recovered]`, `[FRONTIER]`,
`[INFERRED]` and similar). Leave them; do not add new ones. Mark this project's changes with their
lane instead, only where they sit inside code that otherwise follows retail, and give the reason in
the same comment. A marker is a pointer, not a justification.

- `[FIX]` a deliberate departure from retail behaviour to fix a bug; say what retail does.
- `[COMPAT]` behaviour a mod needs that retail does not have.
- `[PERF]` a load-time or throughput change; say what the serial code did.
- `[DEV]` a diagnostic or developer hook.
- `[RENDER]`, `[UI]`, `[TOOLS]` lane code hooked into retail code; new files of these lanes say so
  once in their header instead.
- `[BUILD]` in build files, where a choice differs from OpenBF2's build.

## Flags

- Environment flags select what is fixed for a session (dev hooks, device creation). Anything that
  can change while the game runs is a console variable (`renderer.*`, `tools.*`): tuned values and
  the A/B switches of milestones in development, so the console, scripts, config files and the
  debug menu can all set them.
- Every environment flag and build option this project adds is listed in `docs/eon64/flags.md`.
- `render`: a milestone in development may keep the old path selectable, by a temporary console
  variable (an environment flag only where the switch cannot happen at run time). On acceptance the
  switch and the old path are deleted in the same change.
- `perf`: a change to order, threading or caching has a flag forcing the old path (serial, uncached,
  in the old order) until the owner accepts it, then goes as for `render`. A swap that returns the
  same result (a lookup in place of a scan) needs no flag, only the in-game check.
- `dev`: permanent, off by default, inert when unset.
- `compat`: gates behaviour retail BF2 must not see (for example `BF2_HUD_AUTOVAR`).

## Documentation

`docs/eon64/` records the current state; commit messages record history. Its
[README](docs/eon64/README.md) maps the documents.

- Read `docs/eon64/status.md` first, then only the sections the task needs (`grep -n '^#' FILE`
  gives the outline). Do not read whole documents by default.
- Update the relevant document in the same commit as the change it describes. Each fact lives in
  one place; elsewhere, link to it.
- `docs/archive/` is OpenBF2's documentation, inherited: reconstruction notes and evidence
  that source comments cite. It is reference, not policy and not current state. Do not read or search it unless a task needs that evidence; `.ignore` keeps
  it out of ripgrep searches.

## Upstreaming

A `fix`, or a `build` change that stands alone, can be offered to OpenBF2. Cherry-pick it onto
`kr4uzi/main` on its own branch; from there OpenBF2's policy applies (`git show
kr4uzi/main:AGENTS.md`): its evidence vocabulary, binary evidence for recovered behaviour, and a
linked issue for intentional changes. Record in `docs/eon64/upstream-fixes.md` what was and was not
checked against the retail binary. The owner submits; agents do not.

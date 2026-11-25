# Status

Where each lane stands now: an index. Read this first, then only the document it links to. Update
it in the commit that changes a lane's or a milestone's state. A milestone's tasks and their state
live only in its document; the open milestones table is generated from them
(`tools/docs.py gen`). IDs, states and formats: [GLOSSARY.md](GLOSSARY.md), [WRITING.md](WRITING.md).

| Lane | State | Where it stands | Detail |
|---|---|---|---|
| `render` | active | Milestones 1–11, 13–16, 18, 19 accepted; open ones below | [render/](render/README.md) |
| `tools` | paused (D-2) | `tools-1` tasks 1–5, `tools-3` built; the rest when a render task needs it | [tools.md](lanes/tools.md#milestones) |
| `ui` | active | `ui-1` to `ui-4` accepted; `ui-5` started, `ui-6` to `ui-11` proposed (Q-3 to Q-6) | [ui.md](lanes/ui.md), [ui-menu.md](milestones/ui-menu.md) |
| `perf` (load) | paused | Trace, caches, prefetch, worker compiles; flags retired | [perf-load.md](lanes/perf-load.md#state) |
| `perf` (frame) | parked | `/O2` plugins, `tailField` table; per-draw cost done by `render-9` | [perf-frame.md](lanes/perf-frame.md#state) |
| `compat` | parked (D-3) | rhooks client features when resumed | [compat.md](lanes/compat.md#missing) |
| `fix` | paused | `fix-G` to `fix-O`, `fix-Q`, `fix-R` fixed; `fix-A`, `fix-C` to `fix-F` open (packaging, build) | [fix.md](lanes/fix.md) |

## Open milestones

Several may run at once; each session works in one milestone's document. Generated.

<!-- gen:open-milestones -->
| Milestone | State | Next | Document |
|---|---|---|---|
| `render-23` Water at the shore, water types, caustics, rivers, whitecaps | In progress: tasks 1–7 built, 8–9 planned | 8 (whitecaps), 9 (checks) | [render-23-water-shore.md](milestones/render-23-water-shore.md) |
| `render-22` Far overgrowth | In progress: tasks 1 built, 3–5 planned, 2 deferred | 3 (shadows), 4 (index range), 5 (checks) | [render-22-far-overgrowth.md](milestones/render-22-far-overgrowth.md) |
| `render-21` Tonemap and grade | In progress: tasks 1–2, 4, 6 built, 5 planned, 3 deferred | 5 (checks; a fire) | [render-21-grade.md](milestones/render-21-grade.md) |
| `ui-5` Foundations | In progress: tasks 1 built, 2–4 planned | 2 (libraries vendored), 3 (the draw device), 4 (integration and the menu driver) | [ui-menu.md](milestones/ui-menu.md#ui-5-foundations) |
| `render-17` Far terrain | Paused: tasks 1–5 seen, 6–7 planned | 6 (bakes for the other levels), 7 (from a jet, cost) | [render-17-far-terrain.md](milestones/render-17-far-terrain.md) |
| `render-12` Clouds | Paused: tasks 6 seen, 1–5, 7–8 built, 10 planned, 9 proposed | 10 (night layers; Q-1 first), 9 (low scud, perhaps) | [render-12-clouds.md](milestones/render-12-clouds.md) |
| `tools-1` Views | Paused: tasks 1–2 seen, 3–5 built, 6–8 planned | 6 (settle and freeze), 7 (stamping), 8 (determinism) | [tools.md](lanes/tools.md#tools-1-views) |
<!-- /gen -->

## Awaiting the owner's look

Built and checked by an agent; an entry goes when the owner has seen it. Format:
[WRITING.md](WRITING.md#an-entry-awaiting-the-owners-look).

- **Rivers' standing waves and the foam they carry** (`render-23.7`, contract 46)
  - Where: Kunar, view `river-narrows`
  - Toggle: `renderer.waterFlowSpeed 2.5` a faster river; `renderer.waterFoam 0` the waves alone
  - Look for: crests held still against the current where it runs shallow and fast, breaking into
    foam carried downstream; rocks and piers narrow the flow
  - Known: at the bend the shore train's breaking (`render-23.3`) whitens the river in still water
    too and hides the carried foam
- **Stale effects compile on workers** (`perf` load)
  - Where: any level after a shader edit (a stale effect cache)
  - Toggle: `BF2_PERF_FXASYNC=0` the old path
  - Look for: the old bytecode draws until the new one swaps in; no stall while effects compile
- **Far overgrowth** (`render-22.1`)
  - Where: Kunar, views `og-sw`, `ft-1500`, `edge-e`
  - Toggle: none
  - Look for: trees over the whole map
- **The grade** (`render-21.1`, `render-21.2`, `render-21.4`, `render-21.6`)
  - Where: Muttrah City, views `sun`, `street`; The Falklands `gpm_cq 32`, view `w0` (night)
  - Toggle: `renderer.gradeCurve 0` the old Neutral grade; `renderer.exposureStrength 1` the old full lift (default 0.6)
  - Look for: contrast 1.2, saturation 1.15, exposure bias −0.3, toe 1.5, the rational shoulder;
    dark scenes lifted only partly; night as accepted, a faint grey-blue (`renderer.gradeNight`)
  - Known: a fire is not judged yet (`render-21.5`; `renderer.gradeHighlightShift`, 0.5); indoors
    has no saved view. Tuning: `gradeTemperature`, `gradeTint`, `gradeShoulder`; per range
    `gradeShadowTint`, `gradeMidTint`, `gradeHighlightTint` (`r/g/b`), `gradeShadowSaturation`,
    `gradeHighlightSaturation`
- **Clouds: no step at the handoff** (`render-12`, contracts 32–33)
  - Where: Kashan Desert, views `hb-owner`, `hb-owner-back`; sun `lightManager.sunDirection 0.7/-0.07/0.71`
  - Look for: no step at the 16 km handoff and no far radial edge (one air integral, a cross-fade
    band); edge albedo and the eroded light bake
- **Clouds' ambient along the view** (`render-12`, contract 34)
  - Where: Kashan Desert, views `hb-owner`, `hb-owner-back`
  - Toggle: `renderer.weatherVisibility 60000` and `renderer.weatherMixing 1500` (the level's: 20000, 3500)
  - Look for: shaded sides take the sky behind them
  - Known: the flatness at dusk is Kashan's haze (Q-2)
- **The sky under the deck** (`render-12`, contract 35)
  - Where: Kashan Desert, views `hb-owner`, `hb-owner-back`
  - Look for: gaps between clouds take the clouds' shadow on the haze below the deck, so they no
    longer glow clear past veiled clouds

## Open questions for the owner

Answer by ID ("Q-4: A"); the answer becomes a row in [DECISIONS.md](DECISIONS.md) and the
question goes.

### Q-1 The star catalog

For `render-12.10`. The Yale Bright Star Catalogue has no stated licence. A. Read it from outside
this tree. B. Another catalog with a stated licence (not yet looked for). No recommendation yet.

### Q-2 Kashan's haze at dusk

Far clouds at dusk shade flat under Kashan Desert's dust haze (visibility 20 km, mixing 3500 m):
the air's in-scatter, as modelled ([render-12-clouds.md](milestones/render-12-clouds.md), near
clouds). A. Keep it: a deep dust layer at sunset. B. Thin the level's haze (compare with the
values in the entry above).

### Q-3 Which menu screens go

For `ui-7` to `ui-9`. The browser, demos, mods, co-op and controls stay (PR uses them). In
question: GameSpy account creation and online login (PR's launcher logs in), BF2 stats,
leaderboards, awards and unlocks (still pages in PR's movie: are they reachable?).

### Q-4 The menu's script language

For `ui-5`. A. Lua, RmlUi's own plugin: scripts live in the documents, reload with them,
isolated from the game. B. The engine's embedded Python 2.7: mods already write it; whether a
client runs it before a map is loaded is unchecked, and menu code would share the game scripts'
interpreter. Recommended: A.

### Q-5 The font and the default menu's look

For `ui-7`. A. A plain engine default. B. A PR-like skin, kept private. C. Both.

### Q-6 Whether the default menu documents ship in this repository

For `ui-7`. A. Yes. B. No.

## Open issues

- The default video profile picks 8x antialiasing while the scene runs 4x. Settle on fixed 4x (a
  single-sample HUD back buffer, the scene's own 4x depth; touches device init and reset).
- Cached static casters (the owner's idea, 2026-11-19): keep the far cascades' static casters while
  the snapped cascade holds still, draw moving objects over a copy. Not planned yet.

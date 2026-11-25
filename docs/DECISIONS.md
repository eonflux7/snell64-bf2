# Decisions

The owner's rulings, one row each, named elsewhere by ID (`D-12`). Rows are never renumbered; a
ruling that replaces another says so and the old row gains "(replaced by D-n)". Acceptances are not
here: a milestone's acceptance is its row in [PLAN.md](PLAN.md). Format: [WRITING.md](WRITING.md#a-decision).
Rows D-1 to D-35 were gathered on 2026-11-25 from the documents' inline "(owner, date)" notes.

| ID | Date | Scope | Decision |
|---|---|---|---|
| D-1 | 2026-11-18 | `render` | A Vulkan backend or DXVK interop (per-sample MSAA reads, custom resolves) waits until milestones 8–13 are done |
| D-2 | 2026-11-19 | `tools` | The lane is paused: milestones `tools-1` and `tools-3` cut to what `render-9` needed; the rest picked up when a render task needs it |
| D-3 | 2026-11-19 | `compat` | Parked while the renderer roadmap runs; offline play works without the missing pieces |
| D-4 | 2026-11-19 | `render-9` | `renderer.contributionCull` (pixels on screen, default 3) replaces the distance cull |
| D-5 | 2026-11-19 | `tools-2` | The GPU pass timers and draw counters become the `tools` lane's, switched on at run time while a run, a benchmark or the overlay needs them |
| D-6 | 2026-11-20 | `render-11` | View distance: fog end stays each level's gameplay limit; only how it looks changes, the same in every direction |
| D-7 | 2026-11-20 | `render-12` | The levels' painted cloud layers are replaced by modern clouds, not constrained by the game's approach; the sky is clear until then |
| D-8 | 2026-11-21 | `render-11` | One light, physical: the sun at the ground is the one every lit surface takes; everything else physical under it, no per-level gain |
| D-9 | 2026-11-22 | `render-12` | Paused, the water (`render-16`) first |
| D-10 | 2026-11-22 | `render-12` | Reachable clouds: volumes in the flight space, flown under, into and above; they may hide aircraft and the ground. Replaces the rule of 2026-11-20 that clouds stay at infinity. The GPU budget is set later; until then measured, not capped |
| D-11 | 2026-11-22 | `render-12` | Clouds tuned by eye in game; the planned reference set (Poly Haven HDRIs and their statistics) dropped |
| D-12 | 2026-11-22 | `render-12` | The painted dome's stars and night clouds may go before the night layers come (`render-12.10`) |
| D-13 | 2026-11-22 | `render-11` | Haze realistic, mostly in the distance (it was optical depth 1 at fog end) |
| D-14 | 2026-11-22 | `render-16` | The water shader is rewritten, not patched |
| D-15 | 2026-11-22 | `render-16` | Water geometry stays flat: normals only, no displacement, for now |
| D-16 | 2026-11-22 | `render-16` | The short cascade's foam is weighted by the long cascade's compression, so its period does not show as a tile |
| D-17 | 2026-11-22 | `render-13` | The compressed (Karis) resolve (`render-13.6`) is dropped: no aliasing seen at bright edges with auto-exposure and bloom on |
| D-18 | 2026-11-22 | `render-15` | The shader pipeline is taken ahead of `render-12`: every later milestone edits shaders |
| D-19 | 2026-11-22 | `render-15` | Plain vertex and pixel shaders in place of D3DX effects: deferred to the first task of a move off D3D9 |
| D-20 | 2026-11-22 | `render` | Milestones are accepted on the owner's look. GPU cost waits for one pass once the planned milestones are in: the GPU pass timers and the benchmark paths, each milestone against `render-11`'s baseline |
| D-21 | 2026-11-22 | `render-17` | Cheap ground past the map for air combat, so a jet at altitude never looks down at the void; visual only |
| D-22 | 2026-11-23 | `render-17` | Far terrain baked per level, not generated |
| D-23 | 2026-11-23 | `render-17` | A fictional level is baked from a real place that fits its region |
| D-24 | 2026-11-23 | `render-17` | Two variants tried side by side: real heights, and heights scaled to the map's relief (`renderer.farGroundScaled`) |
| D-25 | 2026-11-23 | `render-17` | Tasks 1–5 fine for now; the milestone paused with bakes for the other levels next |
| D-26 | 2026-11-23 | `render-20` | GTAO (small-scale occlusion) split out of `render-14`; not planned |
| D-27 | 2026-11-23 | `render-21` | The 3D LUT (`render-21.3`) deferred, with per-level looks |
| D-28 | 2026-11-23 | `render-21` | Auto-exposure lifts dark scenes only partly (`render-21.6`): interiors were overexposed and the shade's drama lost |
| D-29 | 2026-11-23 | `render-13` | `renderer.exposureBias` −0.3: Muttrah City's day read washed out at 0 |
| D-30 | 2026-11-23 | `render-12` | A density powder term on the clouds' undersides: tried and dropped |
| D-31 | 2026-11-23 | `render-19` | Hex-tiled detail textures, after a lattice seen on Muttrah City's far hills; Muttrah City's city tiles on the terrain stay on the grid; the cost is not timed |
| D-32 | 2026-11-23 | `render-14` | The sun term is owned at run time everywhere; the lightmaps' sun channel goes. Nothing is authored: each round gets its own weather and sun position |
| D-33 | 2026-11-23 | `render-23` | Water at the shore is physically based throughout. No fade at the shoreline (the flat plane meeting the terrain is the right edge); the shore follows Dagor's approach, its source read first |
| D-34 | 2026-11-24 | `render-23` | The wind blows from the map's most open sea (physical), not the level's authored direction |
| D-35 | — | `render-5` | Retail's per-caster dynamic shadows are gone; the cascades replace them |

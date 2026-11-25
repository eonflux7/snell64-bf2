---
id: render-22
title: Far overgrowth
state: in progress
updated: 2026-11-23
---

# Render milestone 22: far overgrowth

Design, tasks and checks of an open milestone; its order is in [PLAN.md](../PLAN.md#render),
the renderer it builds on in [render/](../render/README.md). On acceptance it is folded into
the subsystem's document there and this file is deleted. Format: [WRITING.md](../WRITING.md).

Overgrowth (PR's placed trees and bushes, `Overgrowth.*` in each level's `overgrowth.con`, the
instances in `overgrowthcollision.con`) ended at the level's `Overgrowth.viewDistance` (Kunar 1000 m),
so past it the map was bare ground, and from higher than that range above the ground there was none
at all. Retail streams a 10 x 10 grid of cells of viewDistance / 4 round the camera: each cell, as
the camera crosses a boundary, is rebuilt from its patch by appending every instance's `_lod` mesh
(a few crossed cards) into one leaf and one trunk stream (16-byte vertices, 16-bit indices), and every
cell's buffers are sized for the densest patch. The near instances (closeDistPercentage, 15 %) draw
their full meshes, pooled per cell.

The design: the far cells cover the whole map, one cell per patch at the level's cell size, built
once at load and never moved, each with buffers sized to its own patch. Drops: the camera-following
ring and its rebuilds. The near full meshes keep the level's range. Cost: memory for the map's `_lod`
geometry (its sum, not the densest patch times the cells); draws, at most two per cell on screen.

The test (`renderer.overgrowthRange`, `5a91eac7`, since removed; read at level load; 0 the level's range): past the
level's range the grid widens at the level's cell size. Kunar (57.7k instances) at 3000 m: 26 x 26
cells of 256 m; GPU 5.73 -> 5.85 ms low over the south-west corner (`og-sw`), 6.00 -> 6.06 ms from
1500 m up (`ft-1500`, none drawn before); 2-3 % fewer frames per second, one sample each; 769 MB of
cell buffers (the densest patch: 48k leaf, 12k trunk vertices). Seen by the owner (2026-11-23:
"looks great"). Changing the range at run time through retail's `reload()` lost every instance.

## Tasks


### render-22.1 Whole-map cells

State: built (2026-11-23).

One cell per patch (slot (x, z) is patch (x, z), `baseUpdateCells`),
all built on the first frame and never re-seated, every cell in range and unfaded; each cell's
streams sized to its patch's placed `_lod` geometry (`createOverGrowthCell(counts)`; retail's
coverage estimate only where the editor generates). `renderer.overgrowthRange` gone. The load logs
`[OVERGROWTH]` (grid, buffers, largest stream). Kunar: 16 x 16 cells of 256 m, 32.7 MB (the test:
769 MB), largest stream 38.7k vertices; trees to the map's edge (`edge-e`); `og-sw` 5.83 ms GPU /
171 fps, `ft-1500` 6.02 ms / 166 fps (retail range: 5.73 / 174, 6.00 / 166). Saaremaa: 12.2 MB,
largest 9.5k. Load time unchanged (in game after 17 s, as before).

### render-22.2 Fade

State: deferred: with task 1 the far cells end only at the scenery frustum (the view distance, past the
map on the levels seen); a dissolve there is needed only where a level's view distance is
shorter than its map.

### render-22.3 Shadows

State: planned.

Far cells past the dynamic cascades' reach cast nothing; overgrowth in the static
cascade (milestone 14's open item) once it no longer streams.

### render-22.4 Index range

State: planned.

A patch over 65535 vertices (a dense level with a long overgrowth range) is
split or logged, not wrapped.

### render-22.5 Checks

State: planned.

Kunar `og-sw`, `ft-1500`; Saaremaa (overgrowth-heavy); Muttrah City and Fallujah West
unchanged near; load time and memory before/after.

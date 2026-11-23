# Render milestone 17: far terrain

Design, tasks and checks of an open milestone; its order and state are in
[PLAN.md](../PLAN.md#render), the renderer it builds on in [render/](../render/README.md). On
acceptance it is folded into the subsystem's document there and this file is deleted.

## State

Paused. Tasks 1–5 built and seen by the owner (2026-11-23: fine for now); Kunar Province, Kashan
Desert and Muttrah City baked. Next: 6, bakes for the other levels; 7, judged from a jet, cost with
the final GPU pass.

Planned by the owner, 2026-11-22: cheap ground past the map for air combat, so a jet at altitude
never looks down at the void. Where it stands: the level's surrounding ring (eight sectors around
the primary heightmap, 8-bit heights, each the primary's size; none on some levels) is lit like the
terrain (sun N.L at its vertex normals, the sky's SH, aerial perspective) but has no shadows and
coarse normals. Past the ring is nothing: the sky shows, and below the horizon the sky repeats its
horizon row (left open by milestone 11). Sea levels already run sea cells to the horizon. Visual
only: no collision or gameplay out there.

- **How far.** With the realistic haze (8 km visibility, 1200 m falloff), a straight ray from
  altitude h down to the ground keeps 5 % contrast up to a slant length of 3 / (σ0 · H/h · (1 −
  e^(−h/H))): about 9 km from 1 km up, 13 km from 2 km, 17 km from 3 km, 25 km from 5 km. A grid
  radius of 30 km covers jets; the curvature drop hides the rest.
- **What is there now** (task 1, 2026-11-22; views `ft-*` on Kashan Desert, Fallujah West, Muttrah
  City, Saaremaa). The rings are not land: on each level they are flat and one colour (Kashan grey,
  Fallujah slate blue), at most a coastline and the river's or roads' meshes running on into them.
  Past the ring: on Kashan the sky's repeated horizon row; on Fallujah West (a desert city) sea cells
  with waves to the horizon (its ring is flat at 0 m, under the river's 22.5 m water level; the
  test's heights were also misread until task 2); on Muttrah City sea cells and ring
  sectors shaded differently, the tiles' squares visible from 2 km. From 600 m up the playable map
  reads as a slab with a cliff at its edge. So the far ground replaces the ring's look too: the ring
  is an input (heights, coast), not something to extend.
- **One draw.** A static camera-centred grid of concentric rings, cells growing with distance,
  snapped to its own cell size so it does not swim. The vertex shader displaces it from one
  height-and-colour texture per level (vertex texture fetch) and lowers it for the Earth's curvature
  (d²/2R, 125 m at 40 km), which hides the outer edge under the haze. It skips the area the ring
  (or the primary map, on levels without one) covers, and its heights are pinned to the ring's edge
  there. Shaded like the ring, by the shared lighting model, with normals from the height texture
  per pixel.
- **Baked per level, not generated** (owner, 2026-11-23). Procedural ground (the known heights
  pulled out under fBm/ridged noise sized from the map's relief, land and water carried out from
  the map's edges, colour from a height-and-slope palette under stochastic tiling) was built and
  dropped after the first look: from altitude it read as invented (strips of land extruded from the
  map's edges, uniform blobs), and no tuning makes it a place. Every level gets a baked file
  instead; until it has one it keeps the retail ring.
- **Real terrain, per level.** Each level ships a baked file: elevation from Copernicus GLO-30/90, coloured by ESA WorldCover
  land classes, each class taking a colour from the level's own colour map (satellite colours would
  clash with the art). Levels are artistic takes on their places, so the bake blends to them over a
  few km. A fictional level is baked from a real place that fits its region (owner, 2026-11-23),
  chosen per level. The bake tool and the per-level files live outside this tree; the engine only
  reads the file if it is there, and without one draws the retail ring. Placement is fitted, not hand-set: the tool searches GLO-90 around the
  named place for the primary heightmap over position, rotation and scale; the fit gives the
  level's latitude, longitude, heading, scale, height offset (a level's 0 m is not sea level) and
  vertical ratio, and a poor fit means the name is borrowed and the level stays generated.
  Two variants, tried side by side (owner, 2026-11-23): unscaled (real heights, a step at the
  blend where real relief is 10-20x the map's) and scaled (heights about a pivot times the fitted
  ratio). One file holds the real heights in the level's datum with the ratio and pivot; a console
  variable picks the variant and the height texture is rebuilt on the CPU (ring near the map,
  blended to the bake), so the shader is unchanged.
- **Which levels are real places** (review 2026-11-23, PR 1.9.0.0, 81 playable levels, by name,
  not yet checked by the fit): about 33 real places of relief like the map's (Fallujah West,
  Khamisiyah, Kashan, Debrecen, Normandy levels, Saaremaa, the Falklands, ...); about 18 real
  places whose real relief far exceeds the map's (Mestia, Bamyan, Korengal, Kunar, Dovre, Adak,
  Muttrah, ...); about 30 fictional, set in a real region. Of the 26 levels with jets, 16 are real
  places and 10 fictional, so the generated ground carries a large share of air combat on its own.
  Heights: about 45 levels use the 0.0025 vertical scale, capped at 163.8 m, and many reach it.
- **Cheap additions.** Cloud shadows from milestone 12's field on the far ground (one texture read);
  a detail texture fading in for a low pass near the edge. Past ~50 km, if needed, a horizon band
  baked into the sky at load.

Tasks:
1. Measure (done, above). Every PR level has nine heightmaps, but not every ring is data:
   Saaremaa's eight sectors are one placeholder file (byte-identical, no ring colormaps; the
   engine gets no ring vertices), Op Barracuda's and Operation Bobcat's are all flat at 0, the
   Falklands' whole cluster is flat (8 km, 64 m cells, sea off: its land is presumably meshes).
   Many rings have flat sectors: Ascheberg, Grostok and Battle of Debrecen 7 of 8, Kafar Halab and
   Operation Soul Rebel 5, Fallujah West and Brecourt Assault 4, others 1-3.
   As built for task 2 (`Geom/FarGround.cpp`, contract 21; `renderer.farGround` 0 draws the retail
   ring): seven levels of 64 x 64 cells, 16 m to 1 km, out to about 32 km from the camera, one draw
   of about 43,000 triangles; each level snapped on the CPU to twice its cell, its outer cells
   morphed onto the next level's grid from 18 to 28 cells out (16 to 26 until 2026-11-23: see below), the levels overlapping by one coarse
   cell. Heights: the ring's vertices into a grid at the ring's cell over its square (769 nodes a
   side on the measured levels), pull-push filled; 3 km past the ring they reach the base height.
   Colour: every patch's colormap shrunk into one texture (512 texels a side at most), read at the
   map's nearest edge and blurred by the distance past it; patches without a colormap take the others'
   mean. Lit by the sun toward `RASM[17]` under the cloud shadow, the sky's SH and aerial perspective,
   normals from the heights per pixel; inside the map the pixels are clipped. The ring had never had
   sun: it read `SUNDIRECTION`, which the engine does not set on the terrain effect, hence its flat
   grey. The ring's heights were misread by the sea test (`measureHeights`: the slot's height stream
   taken for grid positions); fixed. A level without a ring (Saaremaa) gets sea all round when its
   map's lowest point is under the sea level. Left for task 3: the far ground is darker and greyer
   than the map at its edge (the map's detail layers are not in its colour), the smear past the edge
   shows streaks, Fallujah West's sea.
2. The grid, its texture slot and the curvature, drawn over the ring's area too, with a flat ground
   at the base height and the playable map's edge colour: the void gone on every level.
3. Known ground and the sea, as built (2026-11-23; `renderer.farGround`: 0 the retail ring, 1 the far
   ground on levels with a bake, the ring elsewhere (default: no level has one yet), 2 the far
   ground everywhere, a placeholder past known ground: the known heights pulled out, falling to the
   base height over 6 km). Known ground: the map, sampled from the heightmap cluster, and each ring
   sector that is data; a sector with under 1 m of relief, or whose height stream hashes equal to
   another's, is a placeholder, and ring nodes under the sea deeper than 5 m below the map's
   lowest point are filler (Fallujah West: 2 of 8 sectors placeholders, 512 filler nodes; Muttrah
   City 7 of 8 data). Heights in two R32F textures, fine over the ring's square at its cell (769
   a side), coarse 1024 a side out to the map's half side plus 34 km (about 68 m), blended over
   1.5 km inside the fine square's edge; the bake fills the coarse one. A sea level counts only
   within 10 m under the map's lowest point to its highest (-50, -10000, ... mean none); with the
   far ground drawing, sea cells past the map draw only where its heights are under it (Fallujah
   West: the river's mouth, 14 coarse nodes, no sea to the horizon). Colour: each patch's
   colormap times its detail layers' mean albedo (pow(2 x texel, 2.2) over the atlas cell,
   weighted by the patch's mean weights), which was missing from the far colour (darker and
   greyer than the map); not yet judged on a bake.
4. Shading: per-pixel normals and cloud shadows (task 2); the detail fade as built: the detail
   record weighing most along the map's edge, decoded as the terrain does, divided by its mean,
   fading out from 500 to 1500 m from the camera. Not yet judged.
5. The bake file and its reader, the placement fit, and one real-place bake as the test: the
   Falklands (jets; its playable area is the islands), else Kashan or Khamisiyah (jets, low
   relief); the two variants compared where they differ, on a mountain level with jets (Kunar
   Province). Placement as built (2026-11-23, tool outside this tree): an automatic search (masked NCC over
   position, heading and scale on GLO-90) found no real match for Kunar Province (55 km around
   Asadabad) or Muttrah City; levels are placed by hand in a window over the real ground with the
   in-game map overlaid, the fit only refining. Placed: Kunar Province, Kashan Desert, Muttrah City
   (all heading 0, scale 1). The least-squares height ratio means nothing where the shapes do not
   correlate (Kunar 0.19): the scaled variant takes the ratio of the reliefs; on sea levels the
   offset is pinned so the level's sea lands on 0 m.
   The bake file and its reader as built (2026-11-23): `<mod>/settings/eon64-farground/<level>.bin`
   (layout at `FarGround.cpp` `loadBake`), int16 heights in the level's datum on a 60 m grid out
   to 36 km past the map's half side (about 1270 nodes a side, 3.2 MB), the GLO-90 ground at the
   placement resampled at 45 m. Datum: on a coastal level the real sea lands on the level's sea,
   with a sea bed 3 to 40 m under it by the distance from the coast; inland, the real ground's
   median under the known ground lands on the level's. The scaled variant (`renderer.farGroundScaled
   1`; 0 real heights, the default) is about a pivot (the sea, or the known ground's median) times
   the ratio of the reliefs above it (98th percentiles): Muttrah City x0.58, Kashan x0.15, Kunar
   x0.17. Past known ground the pulled-out known heights reach the bake over 3 km. An inland bake
   has no water past known ground (Kunar Province's river level at 36 m would flood the real
   valleys). The load logs the bake's correlation with the map, as placed and mirrored, to check
   the placement's orientation (Muttrah City 0.75 as placed, about 0 mirrored; Kunar 0.48). Seen
   in game on Muttrah City (`ridges`, `ft-top`) and Kunar Province (`ft-1500`, the variant
   switched live). Where it meets the map (2026-11-23, contract 23): the mesh starts at 4 m cells
   (nine levels, about 76,000 triangles with the strip), and an edge height texture at the map's
   cell (the map plus 256 m) holds the map's *drawn* heights: the compiled patches stitch their
   last row to the ring (Kunar Province's east edge drawn 6 m under the heightmap there, 11 m at
   most), so the patches hand their vertices within 64 m of the edge over as they load. Past the
   edge the ground (the ring's heights) is brought to the edge's height, fading out over 160 m
   (Kunar: 3.3 m off on average, 21 m at most). A strip 30 m deep under the edge closes gaps
   where the two surfaces simplify differently at range; the far ground reaches a pixel into the
   map, sunk under it (clip() drops whole pixels under MSAA); the curvature drop starts 2 km past
   the map. Seen on Kunar Province `edge-low`, `edge-e`: no step or gap. The clouds in the haze: [11](../render/atmosphere.md#11-atmosphere-sky-ibl-water-reflections)'s per frame
   (contract 25), reworked by [18](../render/atmosphere.md#18-atmosphere-rework). Colour past the map
   (2026-11-23, bake version 3, contract 29): ESA WorldCover's land class per 30 m node after the
   heights (sea nodes as water). Each class has a reference albedo in the engine
   (`kLandClasses`), tinted by the map's colour under the bake (one gain per channel over the
   classes found there; snow untinted), leaning up to halfway toward the class's own mean under the
   map (full at 3000 texels: placements are borrowed, so a class's own mean is only half
   trusted). The classes go into a 2048 x 2048 sRGB DXT1 texture over the bake's square; the
   shader blends to it from the blurred edge colour over 1.5 km past the map. Seen on Kunar
   Province (`ft-1500`, `jet`, `top-12k`: the valley, forest and bare slopes where the class map
   has them), Kashan Desert (`ft-2000-y0`: bare, villages and fields), Muttrah City (`ridges`).
   Open: from above the map reads darker than the ground round it (its tree meshes; the far
   ground has none); within a class the colour is flat.
   Cracks and crawl (owner, 2026-11-23, Kashan Desert from a jet; built, contract 31). One-pixel
   sky-coloured cracks along level boundaries came and went as the camera moved: where a finer
   level's snap is a coarse cell ahead it meets the coarser level's hole edge to edge, and those
   coarse edge vertices, up to 17 of their cells from the camera, were already morphing (from 16),
   so they slid off the finer level's. The morph now runs 18 to 28 cells. And the ground visibly
   moved as levels snapped and morphed: every vertex read the full-resolution heights, so a
   level's coarse vertices picked single 60 m samples that changed as it snapped. The fine and
   coarse height textures now carry box-filtered mips, and a vertex reads them filtered to its
   distance / 14 (about its level's cell at the hole's edge, twice it once morphed); by distance,
   not level, so coincident vertices of two levels still agree. The edge heights stay full
   resolution (the map's drawn edge). The pixel shader's normals read at their step's mip.
   Seen on Kashan Desert `fg-corner`, `fg-corner2`.
6. Bakes for the other levels, fictional ones from a real place that fits their region.
7. Judged by eye from a jet on the task 1 levels; GPU and CPU cost (one draw) with the final GPU
   pass. Tasks 1–5 seen by the owner and fine for now (2026-11-23).

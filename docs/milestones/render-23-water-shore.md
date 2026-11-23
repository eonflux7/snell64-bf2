# Render milestone 23: water at the shore, water types, whitecaps

Design, tasks and checks of an open milestone; its order and state are in
[PLAN.md](../PLAN.md#render), the renderer it builds on in [render/](../render/README.md). On
acceptance it is folded into the subsystem's document there and this file is deleted.

Builds on milestone 16 ([water.md](../render/water.md)). What the owner sees (2026-11-23):
- Muttrah City, view `ssr-shore`: dark ragged patches with stepped edges along the promenade, tens
  of metres out. Screen-space reflections or the bed under the water.
- The shore's foam is a fixed white strip. Its depth threshold (Hs / 0.78, about 1.5 m at 7 m/s)
  is the open sea's everywhere, so shallow rivers (Kunar) foam over their whole width.
- Whitecaps look uniform and tiled over a wide sea: the 87 m cascade's foam is broken only by a
  40 m noise field.
- The water's colour: `renderer.waterColor` alone sets both the albedo (× 8) and the extinction
  (0.03 / albedo per metre), from a colour the levels authored for retail's blend.

Decided with the owner (2026-11-23): physically based throughout. No fade at the shoreline: the flat
plane meeting the terrain is the right edge. What is missing there is the run-up, wet sand and
breaking foam. The shore follows Dagor's approach (its open-source DagorEngine is read first).

Tasks:

1. **The dark shore patches.** Find out whether they come from the reflection march or from the bed
   (the bed's patches near land, the deep-water test a pixel from sky), with a debug view of the
   water's terms. Fix the cause. On the reflection side the known weak spots are the 16 steps and
   hits on surfaces facing away from the ray.
2. **Open-water map.** Baked at level load from the heightmap and the water level: distance to shore
   (signed, its gradient the direction to the shore) and the fetch upwind. Waves' height and foam
   scale with the fetch, so rivers and harbours stay calm and the shore's breaking depth is local.
3. **Shore waves (Dagor).** Wave fronts travelling up the distance map's gradient, breaking where
   the depth falls under their height, foam from the break; run-up sheets on the beach and a wet
   sand band on the terrain above the waterline. Replaces the fixed shore strip.
4. **Water types.** Pure water's absorption built in, and the level's turbidity (scattering and its
   tint) as a water type from Jerlov's classification, picked from `waterColor` by default with a
   console variable to override. Rivers get their own type.
5. **Whitecaps.** Wind gusts varying over hundreds of metres, and breaking weighted by the 503 m
   cascade's crests too, so no scale reads as a tile.
6. **Checks.** Muttrah City `ssr-shore`, `seahigh`; The Falklands; Kunar's rivers; GPU cost.

# Renderer: sun shadows

Part of [the renderer as built](README.md).

## Sun shadows

`SunShadows.cpp`. The cascades replace retail's per-caster dynamic shadows, which are gone (owner).
Casters come from a **submission context**: each mesh type's normal submission
takes its cull volume and queue target from a pass context, so the shadow pass re-runs submission
against the cascade volume into a caster list (`rendererShadowCasters`). Camera queues are not
reused, because meshes cull per material against the camera. Casters draw with each Ra effect's
`RaShadowDepth` technique; terrain draws its own (`Terrain::drawShadowCasters`).

Objects come from the object manager's roots-only grid query against the light volume (the
light-space box around the four cascade spheres); hierarchies are expanded by the drawer. Seen in
game on Muttrah (`2860a74a`): a supply crate's shadow no longer depends on view direction. Objects
have no distance cull (milestone 8), so a caster never depends on the camera's distance to it.

Each cascade draws only the casters that can shadow a receiver in its own slice
(`ShadowCascadeCull`): in light space, the 2D convex hull of the slice's eight corners, pushed out by
the receivers' filter (2.5 cascade texels) plus their normal offset, and in depth from the projection's
sun-side bound to the slice's farthest corner. Casters carry a world bounding sphere, the geometry's
local box sphere through its world transform; one without bounds draws in every cascade.

The gather applies the same test once for the whole shadow range (the hull of the slice from the
near plane to the shadow distance, the widest cascade's margin, the nearest sun-side bound), which
holds every cascade's own cull, before any work on an object: an object stays when its visual
bounding sphere (the grid query's) or one of its geometries' spheres touches the range. Only those
are brought up to date (visual update, geometry update) where the camera pass did not see them, and
submitted. The light volume holds 1.5 to 2.3 times as many objects as reach a cascade.

- Four cascades over the view depth up to 400 m, split by a blend of
  logarithmic and uniform (lambda 0.75). Each cascade but the last also covers the first 20 % of
  the next slice; there the receiver blends from the finer cascade to the coarser one, so splits
  don't show.
- Each cascade is a bounding sphere of its view slice, so its size does not change as the camera
  turns, and its centre is snapped to whole texels across the light, so static shadows don't crawl.
- Casters up to 500 m toward the sun beyond the sphere still draw.
- The atlas stores the light-space depth of the nearest caster (0 = sun-side bound, 1 = far side).
  Receivers compare and filter (3 x 3 tent PCF) and, past 90 % of the last split, blend to the
  baked lightmap sun.

Tuned live through console variables, which clamp to their ranges and default to the values the
milestone was accepted with:

| Variable | Default | Range | What |
|---|---|---|---|
| `renderer.sunShadows` | 1 | 0–1 | Cascades drawn; off, surfaces take the baked lightmap sun |
| `renderer.sunShadowDistance` | 400 | 50–2000 m | View depth the cascades reach |
| `renderer.sunShadowResolution` | 2048 | 512–4096 | Texels per cascade side, a power of two; a change re-creates the atlas |
| `renderer.sunShadowCasterReach` | 500 | 0–2000 m | Caster reach toward the sun past a cascade's sphere |
| `renderer.sunShadowSplitLambda` | 0.75 | 0–1 | Splits: 0 uniform, 1 logarithmic |
| `renderer.sunShadowCascadeBlend` | 0.2 | 0–0.5 | Part of the next slice each cascade also covers and blends over |
| `renderer.sunShadowNormalOffset` | 1.5 | 0–4 texels | Receiver offset along its normal |
| `renderer.sunShadowDepthBias` | 1.0 | 0–4 texels | Receiver offset toward the sun in depth |

Their help lines, for the debug menu, come from `rendererVariableHelp` (`RendererConsole.cpp`).

### Cascade constants (`RASM`, 22 float4, pixel registers c200–c221)

| Rows | Contents |
|---|---|
| 0–3 | Camera: position; forward; right / P00; up / P11 |
| 4 + 3i .. 6 + 3i | Cascade i: world to atlas texel (x, y) and light depth |
| 16 | View depth each cascade's slice ends at |
| 17 | `.xyz` direction toward the sun; `.w` view depth where the blend to baked sun starts |
| 18 | Each cascade's texel size in world units |
| 19 | Each cascade's light depth per metre |
| 20 | `.x` one atlas texel in UV; `.y` normal offset and `.z` depth bias, in cascade texels |
| 21 | View depth each cascade's coverage ends at (its blend into the next slice) |

All zero outside the main world pass, and when no cascade was rendered: nothing is covered.

The engine owns the registers: it writes them once per frame through the StateManager's constant
mirror (`stateManagerSetSunCascadeConstants`), and the StateManager drops the part of any effect's
pixel constant write that falls in the range. Each receiver declares `RASM` at `register(c200)`,
so D3DX would otherwise upload the parameter's own zeros at every pass. No pixel shader may place
anything else there.

## 5. Sun cascaded shadow maps

See [Sun shadows](#sun-shadows). Accepted at the values its console variables now default to.

## 14. Run-time sun term (the lightmaps' sun channel dropped)

Planned by the owner, 2026-11-23; reviewed and replanned the same day. The sun term is owned at run
time everywhere; the lightmaps' sun channel (`g`) goes for terrain, roads, static meshes, grass and
water. Sky visibility and baked point lights stay baked. Lightmaps are not re-baked.

Why: nothing is authored (owner, 2026-11-23). Each round gets its own weather, sun position and
clouds (the clouds are already randomized), so the level's own sun is just one case among many and
the baked term, visibility × N.L for that sun, is wrong almost every round. Found on Kashan Desert
with the sun at 4° (`lightManager.sunDirection`): past the cascades the map keeps its noon light
while the cascades and the far ground take the real cosine, so a dark disc round the camera the
size of the cascades' reach and a bright map against a dim far ground. Run-time shadows also give
the far ground its first shadows and vehicles shadows past 400 m. A shortcut was weighed and
dropped: dividing the baked N.L out of `g` and multiplying by the live one fixes the brightness, but
leaves the shadows pointing at the authored sun.

The sun is fixed for a round (it need not move during one), so everything sun-dependent is drawn
once at level load, after the round's sun is chosen. Choosing it belongs with milestone 18's
weather per level; until something does, the sun is set with `lightManager.sunDirection`, and a
console change redraws everything at once (a hitch is fine for a dev tool).

Three layers of sun visibility, nearest first:

- The cascades (as now, to 400 m).
- **One static cascade over the map**: terrain, static meshes, trees (leaf and trunk, alpha-tested)
  and overgrowth. Its projection is fitted to the map's bounds in light space, not a fixed texel
  size: at a 4° sun a 4 km map is about 280 m deep in light space. 4096² by default
  (`renderer.sunStaticResolution`). Casters at fixed LODs, independent of the camera (today's
  casters take camera LODs, and the terrain's come from the camera's culler,
  `Terrain::gatherShadowCasters`), so the cache never depends on where the camera is. Its own
  normal offset and depth bias: the cascades' 1.5 texels would be 1.5 m, and leak through walls and
  under roofs. Drawn at load and when the sun changes; a static object destroyed or added (PR's
  buildings and bridges) marks its region for redraw. 64 MB R32F; its D24S8 target only while
  drawing (freed or shared with the cascades). Shares its caching with the cached static casters
  idea ([STATUS.md](../STATUS.md#open-issues)).
- **The far ground's shadow height**: per texel, the world height below which the sun is blocked
  by terrain, marched toward the sun over the far ground bake with the map's heights included (at a
  low sun, mountains past the map shadow into it, and the map into them). A height, not a
  visibility, so anything above the ground (aircraft) reads it right with one fetch. Coarse, over
  the far ground's extent; on the map the static cascade already holds the terrain. Levels without
  a far ground bake have none.

Past all three the sun is unshadowed N.L.

Where the baked term enters today, each a `lerp(baked, cascades, weight)`: `Ra_ResolveSunVisibility`
and `Ra_ResolveSunCosine` (`rasunshadow.fxh`; static meshes, terrain, roads, decals, and vehicles,
which pass 1), water (`rashaderwaterbase.fx`), grass (`GetGrassSunTerm`, `undergrowth.fx`). The
layers go inside those functions, so every receiver that calls them gets them, vehicles and decals
included. Particles stay on the hemi map's alpha: out of scope. Thermals keep the baked term.

Accepted loss: the baked soft building shadows at range; far shadows are hard-edged and about 1 m.

**As built** (2026-11-23, `SunStatic.cpp`, contract 36; accepted by the owner the same day, no
lightmapped sun to go back to):

- **Budget.** `RaSunStatic`, eight engine-owned pixel registers c180–c187 below the old range
  (`kEngineRegisterBegin` 180), published with `RASM`, zero outside the main world pass. Checked
  against every effect's own allocation: the 611 cached Ra effects (the `.cfx` cache decoded, its
  ps_3_0 constant tables read) use at most c15 for their own constants, the 25 legacy effects with
  bytecode at most c53 (`tools.survey`, which reports any effect constant in c180–c223). Both
  layers share one R32F atlas on s12, bound as the world pass publishes (the frame steps before it
  take s12 for their own samplers), and on s3 while water draws (its s12 is the scene copy).
- **Receivers.** `Ra_SunVisibility` (`rasunshadow.fxh`) behind `Ra_ResolveSunVisibility`,
  `Ra_ResolveSunCosine`, grass and water: in the main world pass (`RaSunStatic[4].w` 1) it replaces
  the baked term everywhere with the round's N.L, the cascades near, the static
  cascade past them, unshadowed past both, all times the shadow height. The far ground takes the
  round's N.L and the shadow height.
- **Static cascade.** Orthographic over the level's box in light space (the map's square, its
  lowest ground − 20 m to its highest + 150 m), 4096² (`renderer.sunStaticResolution`); its axes
  scale separately, so at a low sun the up axis, along which the ground recedes, is the finer
  (Kashan at 4°: 1.41 m across, 0.16 m along). Casters: terrain at full detail and the static
  meshes of objects without mobile physics (`ObjectDrawer::submitShadowCasters`, `staticOnly`), at
  the LOD a fixed distance gives (one camera pixel per texel at the screen's height, about 1100 m
  at 1.4 m texels; `rendererCasterLodDistance`). Overgrowth streams round the camera and is left
  out. Receiver offsets in texels: along the normal (`renderer.sunStaticNormalOffset` 1) and
  toward the sun (`renderer.sunStaticDepthBias` 1) times 1 + the receiver's slope to the sun (tan,
  at most 8). Small casters (walls, tents) keep a shadow at 30°; acne free on Kashan. A destroyed
  state (`StaticMesh::setGeom`) or a static mesh's destruction redraws the static cascade alone, at
  most once a second; static meshes created during a round are not reported (overgrowth's pools
  create them as the camera moves), so deployables placed in a round cast nothing past 400 m.
- **Shadow height.** 512² over the far ground's coarse heights (70 km, 141 m texels), marched on
  the CPU (8 threads; the step 0.5 cell + 1 % of the distance): per texel the highest h(s) − rise·s
  toward the sun from 1.5 cells out, less 1 m + rise · cell; on the map only terrain off it.
  Bilinear in the shader with a soft band of rise × texel (at least 2 m). None without a far
  ground.
- **Cost at a sun change** (Kashan Desert, CPU): the static cascade 2–20 ms to gather (Muttrah
  City, 5520 casters, 19.6 ms), the height march 4 ms at noon and 95 ms at a 4° sun. Memory: the
  atlas 4096 × 4608 R32F, 75 MB; its D24S8 depth (64 MB) only while drawing. GPU time not taken
  (with the final GPU pass).
- **Seen by the agent.** Kashan Desert at a 4° sun from 600 m: the map no longer a bright slab
  against the far ground, ridges throw long shadows (`m14-4deg-baked` / `m14-4deg-run`). Static
  cascade alone (`renderer.sunShadows 0`) at a 30° sun, view `m14-above`: wall, hut and tower
  shadows as the cascades draw them, coarser. Muttrah City `sun`, `ridges` at the level's sun:
  the same as baked, the soft baked building shadows at range gone. Saaremaa `water` at a low sun
  (clouds off): the glint as baked, the island's shadow over the bay.

Tasks:

0. Budget: done (contract 36).
1. Run-time N.L past the cascades: done.
2. Static cascade: done, without overgrowth and without casters added during a round.
3. Far ground shadow height: done.
4. Checks: done as above, but frame time with the final GPU pass.
5. On acceptance: the baked sun fallback and its switch (`renderer.sunBakedFallback`) deleted: done.

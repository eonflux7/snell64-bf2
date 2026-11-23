# Renderer: terrain

Part of [the renderer as built](README.md).

## Terrain

Render milestone 10 (accepted by the owner, 2026-11-20). Terrain draws in one pass at full detail
everywhere. Retail drew a lightmap pass (`ZFillLightMap`: depth and the lightmap packed to `[0,1]`
into an accumulation target, `+0x774`), additive point-light passes into that target, then per
patch one additive pass per detail layer on the near nodes and a low-detail pass on the far ones,
each reading the target projected and fogging again where they overlapped. Roads read the same
target.

- **Atlases** (`Geom/TerrainAtlas.cpp`, built on the terrain's first `update` after a load,
  managed pool): one detail atlas per (format, size) of the level's detail and side textures, each
  cell the texture 2x2 shifted half a tile, copied per DXT block, so the shader wraps it
  (`atlasUv = frac(uv) * scale + offset`, `tex2Dgrad` with the unwrapped gradients); one lightmap
  atlas of every patch's lightmap, cell = patch grid position. Console `terrain.atlasDump` writes
  them as `.dds`.
- **Depth**: the visible patches draw `RaDepthPrepass` at the end of the depth prepass
  ([Frame](README.md#frame), item 3), then the far ground `RaFarGroundDepth` (contract 24).
- **`Terrain::drawMain`** (the +0xB8 arm) flushes the aux queues and gathers the point lights per
  patch (`gatherPointLights`: the culler's sphere gather per light, the nearest four to the patch
  centre; the static frame lights under ShaderVersion 1, else the visible lights of mask 2313).
- **`Terrain::drawFinal`** (the +0xBC arm) draws technique `RaTerrain`, one draw per patch over its
  visible nodes' LOD 0 ranges (`QuadPatch::drawNodes`, adjacent ranges merged), shader contract 5:
  up to six layers in layer order from the atlases, blended by weight; the low-detail map modulates
  them, fading in with distance; fog once on the weight-normalised colour; layer 6 (under water)
  blends the water colour by depth. Light (contract 4): the patch lightmap at the colormap's UV
  (centroid), the sun cascades, the per-patch point lights. Without the atlases or the technique
  terrain draws no colour.
- **Roads** (compiled roads, `roadcompiled.fx`; road static meshes, `rashaderroad.fx`) light from
  the lightmap atlas by world position (`Ra_SampleTerrainLightAtlas`; grid and (GI colour, 1) in
  the Ra records `TerrainLightAtlasGrid`/`TerrainLightAtlasGI`, retail's accumulation projection
  pair, and on compiled roads `TEXPROJSCALE`/`TEXPROJOFFSET`). No distance cull and no fade: they
  draw to the far plane. Dynamic point lights do not reach roads.
- **Gone**: the accumulation target's use and its `AccumulationMap` record, `Hi_Terrain` and its
  passes, the culler's detail distance and `detailNear`, the console properties
  `terrainCuller.lodSwitchDistance` and `lod0SwitchStart` (the road view distance and fade) and
  the video option's culler distances, the low-quality branches and the Renderer's terrain-quality
  slots (+0x98 does nothing, +0x9C answers false), the dynamic shadow-range index buffer (+0x368).

Frame time on the benchmark paths, old path against the one pass (same build and session,
Conquest 64, 1920x1080 4x, 3 runs each, 2026-11-20): Muttrah City `bench1` 7.81 to 7.92–8.02 ms
(draws 3324 to 3465, Mprims 3.18 to 2.91), Fallujah West `bench` 14.92 to 15.17–15.27 ms (4481 to
4489, 2.88 to 2.75), Saaremaa `bench1` 5.18 to 5.20–5.21 ms (1640 to 1630, 2.56 to 2.29; the frame
GPU span 4.74 to 4.38–4.48 ms). Every frame CPU bound; the 0–2 % is within the spread between runs.
After the deletion: 8.39 / 8.48 (another load on the machine during the run), 15.34 and 5.22 ms.

## 10. Terrain in one pass

Accepted by the owner, 2026-11-20; as built in [Terrain](#terrain).
Survey of the three maps (task 1, 2026-11-20; console `terrain.survey`):

| | Muttrah City | Fallujah West | Saaremaa |
|---|---|---|---|
| Patches (128 m) | 8x8 | 4x4 | 8x8 |
| By layer count 1/2/3/4/5/6/7 | 15/17/1/2/17/5/7 | 0/3/3/2/5/2/1 | 34/0/2/7/14/5/2 |
| With planar / env / layer 6 | 46 / 0 / 39 | 16 / 0 / 4 | 30 / 0 / 56 |
| Detail textures | 6, 512² m7, DXT1/DXT5; detail 0 planar | 6, 512² m7, DXT1/DXT5, all planar | 6, 512² m7, DXT1/DXT3/DXT5; detail 0 planar |
| Weight maps | 256², R5G6B5 or DXT1, 1 mip | same | same |
| Colormap / lightmap / low-detail | 512² DXT1 / 1024² DXT1 / 256² DXT1 | low-detail mode 3, 512² R5G6B5 | 512² DXT1 / 1024² DXT1 (32 patches 8x8 dummies) / 256² DXT1 |
| Point lights on terrain | none (`drawMaskLights` path, shader version 0) | none | none |
| `bench1`/`bench` frame | 10.66 ms | 20.51 ms | 6.34 ms |
| GPU: lightmap / depth / detail | 0.398 / 0.168 / 0.337 ms | 0.490 / 0.200 / 0.623 ms | 0.312 / 0.145 / 0.321 ms |
| GPU: roads | 0.095 ms | 0.102 ms | 0.014 ms |
| Draws a frame: pass 0 / 3 / 4 / 5 / 14 | 12 / 13 / 18 / 3 / 3 | 9 / 6 / – / 24 / – | 18 / 17 / 2 / 1 / 3 (average) |

Patches with one layer and no colormap are sea or off-map (Saaremaa's 34). No environment-mapped
layer and no point light reaches terrain on the three maps. The GPU and frame rows are the old
path, measured with `GPU=1`.

## 19. Hex-tiled detail textures

Planned by the owner, 2026-11-23, after a lattice seen on Muttrah City's far hills: tiled textures
repeat on a square grid the eye picks out at range. Replaces the plain tiled lookup with hex-tiling
(Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022): the plane is cut into hexagons about 0.6
tiles across, each reading the texture shifted by its own hash; the three around a pixel blend with
weights sharpened (exponent 7) and steered by their luminance (0.6), so contrast holds and no grid
repeats. A tap under 0.4 % of the weight is not read, so most pixels read one or two taps, not
three (the gradients are passed in, so the branch is safe). Retail's low-detail modulation stays on
top as the large-scale variation. Chosen over histogram-preserving stochastic texturing (needs a
preprocessed texture and LUT per layer, at odds with the atlases) and over two-tap noise variation
(blends show on contrasty textures). `renderer.hexTiling` (1) switches it; 0 tiles as retail.
The lattice is the shaders' `RaHex.fxh`, shared by the terrain and static meshes; contract 30.

- **Which textures.** A shift breaks a pattern laid on a grid and a lookup in a strip atlas, and
  which textures those are is PR's content, so a list outside the tree says
  (`<mod>/settings/eon64-hextiling.txt`, `Geom/HexTilingList.h`, read at each level load):
  `[terrain grid]` names terrain layers kept on the square grid (Muttrah City's octagon slabs,
  tiles, cobbles, crop rows; every other layer is hex-tiled); `[static hex]` names the static mesh
  detail textures hex-tiled (every other stays on the grid). There a name alone is a whole sheet;
  with V ranges, only those strips of a strip atlas (most of PR's building textures: plaster,
  bricks, tiles and signs stacked in strips that tile along U), shifted along U only so a lookup
  never leaves its strip. `/N` marks a sheet or strip laid on a grid of N tiles (roof slabs,
  concrete panels): its shifts step by 1 / N tile, so the joints stay continuous and only which
  slab shows varies.
- **Static meshes.** `RaShaderSTM`'s detail map and detail normal map read the same taps; the
  luminance steering comes from the colour. The material's `RaHexDetail` is set when its textures
  bind and applied with the mode zeroed while `renderer.hexTiling` is 0. Bundled meshes have no
  detail layer (one diffuse map on unique uv), so they are not touched.

- **No turn.** The reference turns each hexagon too; on PR's rock and sand that points their
  streaks every way (Muttrah City `mountain`: hairy, blotchy), so hexagons are only shifted, top
  plane and sides alike. Strata stay level.
- **Cost.** Up to three reads per plane where one was; not timed (owner, 2026-11-23: not needed).

Tasks:
1. Terrain and far ground (built 2026-11-23): every detail layer's planes in `RaTerrain`, and the
   far ground's detail fade (which carried the lattice: Muttrah City `valley`, the hills behind
   the mountain, A/B with `renderer.hexTiling 0`).
2. Static meshes (built 2026-11-23; owner: rooftops) and the list: Muttrah City's city tiles on
   the terrain kept on the grid (owner, 2026-11-23); `RaShaderSTM`'s detail by whole sheet, by
   strip, and in grid steps. Muttrah City `roofs` (city_paving_de and mecity_01_de's panels on the
   roofs, its plaster on the walls), `market`.
3. Judged by eye: accepted on Muttrah City (owner, 2026-11-23); no GPU timing (owner).

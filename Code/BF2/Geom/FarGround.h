// [RENDER] The far ground (render milestone 17). Not part of retail BF2.
//
// Ground from the playable map's edge out to about 32 km, in place of the surrounding ring, so the
// land goes on wherever the camera is (jets at altitude). One draw: a static mesh of nested square
// levels around the camera, each twice the cell of the one inside it (16 m to 1 km cells), snapped
// to its own grid on the CPU so it does not swim, the vertex shader placing it by height texture
// fetch and lowering it by the Earth's curvature. The fine levels' outer cells morph to the next
// level's grid, so neighbouring levels meet without cracks; they overlap by one coarse cell.
//
// What is built at load (on the terrain's first update, after the atlases):
// - Known ground: the playable map (the heightmap cluster's heights) and each surrounding sector
//   that is data. A sector that is flat or byte-identical to another (placeholder files) is
//   unknown, as are ring nodes under the sea deeper than the map's lowest point (filler), and
//   everything past the ring.
// - Heights, two R32F textures: a fine one over the ring's square (three map spans a side, at the
//   ring's cell) and a coarse one out past the mesh's reach. Known ground is kept; past it, the
//   known heights pulled out (pull-push) blend over 3 km to the level's bake: real terrain in the
//   level's datum, from <mod>/settings/snell64-farground/<level>.bin (written by a tool outside this
//   tree; layout at loadBake). renderer.farGroundScaled picks its variant, real heights or scaled
//   about a pivot to the map's relief, and the heights are rebuilt on the CPU when it changes.
//   Without a bake, a placeholder: the pulled-out heights falling off to the level's base height.
// - The sea: a sea only when its level lies near the map's height range (levels set -50, -10000
//   and the like for none). Water cells past the map draw only where the heights are under it
//   (farGroundSeaIn).
// - Colour: the map's colormaps shrunk into one sRGB texture with mips, each patch's cell scaled by
//   the mean albedo of its detail layers (the map's colour is the colormap times them). Past the
//   map the shader reads it at the map's edge, blurred with the distance. Near the camera the
//   map's dominant edge layer's detail texture fades in (low passes).
//
// Where it draws: renderer.farGround 1 (the default) on levels with a bake, the retail ring
// elsewhere; 0 the ring everywhere; 2 the far ground everywhere (placeholder heights without a
// bake). Procedurally generated ground past the ring was tried and dropped (owner, 2026-11-23):
// levels are baked from real terrain, fictional ones from a real place that fits their region.
// Shader side: technique RaFarGround on the terrain effect (contract 22).
#ifndef BF2_GEOM_FARGROUND_H
#define BF2_GEOM_FARGROUND_H

namespace dice {
namespace hfe {
namespace geom {

class Terrain;

// A terrain is loading: drops what was built for the last one.
void farGroundLoad(Terrain* terrain);
// Surrounding slot `slot`'s vertices as it loads: world x, z and height, `count` of each, the
// ring's span (m, one map side) and cell (m).
void farGroundAddRing(int slot, const float* x, const float* z, const float* y, unsigned int count, float span,
                      float cell);
// A map patch's drawn vertices near the map's edge as it loads (world x, z, height, `count` of each):
// the far ground meets these, not the physics heightmap, where they differ.
void farGroundAddMapEdge(const float* x, const float* z, const float* y, unsigned int count);
// The terrain's first update after its load, once its textures exist: builds the textures and the
// mesh. Later calls do nothing.
void farGroundUpdate(Terrain* terrain);
// Draws the far ground; false when it is off (renderer.farGround 0) or not built, so the caller
// draws the ring instead.
bool farGroundDraw(Terrain* terrain);
// Draws the far ground's linear view depth (technique RaFarGroundDepth) in the depth prepass, after
// the map's terrain, so what reads the scene's depth sees it; nothing when it is off or not built.
void farGroundDrawDepth(Terrain* terrain);
// Whether the far ground is built and on; then the sea past the map follows farGroundSeaIn.
bool farGroundActive();
// Whether the far ground's heights fall under the sea anywhere in the world xz box (the box is
// clamped to the heights' extent, past which they hold their edge). False without a sea.
bool farGroundSeaIn(float x0, float z0, float x1, float z1);
// The level's height above sea level from its bake (version 2): real elevation = level height +
// `offset` (m). False without a bake or with a version 1 bake. Render milestone 18's altitude.
bool farGroundBakeSeaOffset(float& offset);
// [RENDER] Render milestone 14: the coarse heights as drawn (before the curvature's drop): n x n
// nodes, row z, node (i, j) at (origin + i * cell, origin + j * cell); the map inside them. A new
// `generation` each time they are rebuilt. False when the far ground is not built.
bool farGroundCoarseHeights(const float*& heights, int& n, float& origin, float& cell,
                            unsigned int& generation);
void farGroundRelease();

} // namespace geom
} // namespace hfe
} // namespace dice

#endif

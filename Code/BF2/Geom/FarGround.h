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
// - Heights: one R32F texture over the surrounding ring's square (three map spans a side) at the
//   ring's own cell, filled from the ring's vertices (captured as each surrounding slot loads; its
//   grid gets coarser away from the map) and pull-push filled between them. Past the ring the
//   height falls off to the level's base height (the median of the ring's outer edge).
// - Colour: the playable map's colormaps shrunk into one sRGB texture with mips; past the map the
//   shader reads it at the map's edge, blurred (a coarser mip) the further out it is.
//
// The ring's draw is the old path, kept selectable by renderer.farGround 0 while the milestone is
// in development. Shader side: technique RaFarGround on the terrain effect (contract 21).
#ifndef BF2_GEOM_FARGROUND_H
#define BF2_GEOM_FARGROUND_H

namespace dice {
namespace hfe {
namespace geom {

class Terrain;

// A terrain is loading: drops what was built for the last one.
void farGroundLoad(Terrain* terrain);
// A surrounding slot's vertices as it loads: world x, z and height, `count` of each, the ring's
// span (m, one map side) and cell (m).
void farGroundAddRing(const float* x, const float* z, const float* y, unsigned int count, float span, float cell);
// The terrain's first update after its load, once its textures exist: builds the textures and the
// mesh. Later calls do nothing.
void farGroundUpdate(Terrain* terrain);
// Draws the far ground; false when it is off (renderer.farGround 0) or not built, so the caller
// draws the ring instead.
bool farGroundDraw(Terrain* terrain);
void farGroundRelease();

} // namespace geom
} // namespace hfe
} // namespace dice

#endif

// [RENDER] Terrain texture atlases (render milestone 10, task 2). Not part of retail BF2.
//
// The one-pass terrain needs every detail layer of a patch in one draw, more textures than
// ps_3_0's 16 samplers hold, so the level's textures are copied at load into a few atlases:
//
// - Detail atlases: the detail records' top and side textures, one atlas per (format, size). Each
//   entry has a cell twice its size holding the texture 2x2, shifted by half a tile, so the tile
//   sits in the middle with half a tile of its own wrapped content on every side, at every mip.
//   The copy is per block, so compressed formats stay compressed and exact. A shader wraps the
//   layer coordinate itself and samples with the unwrapped coordinate's gradients:
//       atlasUv = frac(uv) * entry.scale + entry.offset
//       tex2Dgrad(atlas, atlasUv, ddx(uv) * entry.scale, ddy(uv) * entry.scale)
//   The pad is half a tile at every level, so bilinear and anisotropic footprints up to half a
//   tile wide stay inside the cell. The last level keeps an 8-texel tile (4-texel pad).
// - Lightmap atlas: every patch's lightmap side by side, column = patch x, row = patch z (the
//   patch index order), so roads and other world-space users look a lightmap up by world position
//   across patches. Patches with a smaller lightmap (8x8 dummies at sea) are scaled up by block.
//
// The atlases are managed-pool textures (no device-reset handling), built on the terrain's first
// update after it loads (once the texture manager has created the textures) and released with
// the terrain.
#ifndef BF2_GEOM_TERRAINATLAS_H
#define BF2_GEOM_TERRAINATLAS_H

#include <string>
#include <vector>

struct IDirect3DTexture9;

namespace dice {
namespace hfe {
namespace geom {

class Terrain;

// Where a detail texture sits: atlasUv = frac(uv) * (scaleU, scaleV) + (offsetU, offsetV).
struct TerrainAtlasEntry
{
    int   atlas;      // index into TerrainAtlases::detail; -1 when the record has no texture
    float scaleU;
    float scaleV;
    float offsetU;
    float offsetV;
};

struct TerrainAtlases
{
    std::vector<IDirect3DTexture9*>  detail;         // one per (format, size)
    std::vector<TerrainAtlasEntry>   top;            // per detail record (Terrain::m_detailTextures)
    std::vector<TerrainAtlasEntry>   side;           // per detail record; equal to top when shared
    std::vector<unsigned char>       grid;           // per detail record: 1 tiled on the square grid, not
                                                     // hex-tiled (render milestone 19, HexTilingList.h)
    IDirect3DTexture9*               lightmap;       // patchCount x patchCount cells, or 0
    int                              lightmapCell;   // texels per patch side at level 0
    // World position to the lightmap grid, for world-space lookups (roads): (1 / patch width in x,
    // in z, patches a side, cell texels); grid = world.xz * (x, y) + patches / 2, the cell is
    // floor(grid) and a patch's edge sits on its edge texels' centres.
    float                            lightmapGrid[4];
    int                              patchCount;
};

// A terrain has loaded: drops the atlases built before. Its textures are created later, in the
// texture manager's post-load sweep, so the build waits for terrainAtlasUpdate.
void terrainAtlasLoad(Terrain* terrain);
// Per frame (Terrain::update): builds the atlases of a terrain loaded since the last call.
void terrainAtlasUpdate();
void terrainAtlasRelease();
// The current atlases, or 0 before a terrain has loaded.
const TerrainAtlases* terrainAtlases();
// Detail atlas `index` as an effect texture value (an engine texture record wrapping it, for
// ShaderParameter::setValue), or 0.
const void* terrainAtlasDetailValue(int index);
// The lightmap atlas as an effect texture value, or 0.
const void* terrainAtlasLightmapValue();
// [RENDER] Roads' own light (render milestone 10): with a lightmap atlas built, the atlas as an
// effect texture value, `grid` = lightmapGrid and `giDirect` = (the terrain's GI colour, 1);
// otherwise 0, leaving `grid` and `giDirect` as they were.
const void* terrainRoadLight(float grid[4], float giDirect[4]);
// [DEV] Console `terrain.atlasDump`: every atlas as a .dds under `dir`, and a log line per entry.
void terrainAtlasDump(const std::string& dir);

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_GEOM_TERRAINATLAS_H

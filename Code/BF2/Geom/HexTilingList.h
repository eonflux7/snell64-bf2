// [RENDER] Which detail textures are hex-tiled (render milestone 19). Not part of retail BF2.
//
// Hex-tiling shifts the texture in each hexagon, which breaks a pattern laid on a grid (paving
// slabs, cobbles, tiles, crop rows) and an atlas of strips (a lookup crossing into the next strip).
// Which textures those are is the mod's content, so it is read from outside this tree:
// <mod>/settings/eon64-hextiling.txt, one texture per line under a section header:
//
//   [terrain grid]    terrain detail textures tiled on the square grid; the others are hex-tiled
//   [static hex]      static mesh detail textures hex-tiled; the others are tiled on the grid. A
//                     name alone hex-tiles the whole sheet; a name followed by up to four V ranges
//                     ("common_01_de 0-0.244 0.62-0.78") only those strips of a strip atlas,
//                     shifted along U only, so a lookup never leaves its strip. A texture laid on a
//                     grid of N tiles (slabs, panels) takes shifts in steps of 1 / N tile, so its
//                     joints stay continuous: "/N" after a range for that strip, alone for the
//                     whole sheet ("city_paving_de /4", "mecity_01_de 0.8037-1/2")
//
// A texture is named by its file name without directory or extension, any case
// ("detail_muttrahslabs"); `#` starts a comment. No file: every terrain layer hex-tiled, no static
// mesh. Read on first use and again with each terrain load (terrainAtlasLoad).
#ifndef BF2_GEOM_HEXTILINGLIST_H
#define BF2_GEOM_HEXTILINGLIST_H

#include <string>

namespace dice {
namespace hfe {
namespace geom {

enum HexTilingSection
{
    kHexTerrainGrid,   // [terrain grid]
    kHexStaticHex      // [static hex]
};

// Re-reads the file.
void hexTilingListLoad();
// Whether the texture at `path` (any directory, extension and case) is listed under `section`.
bool hexTilingListed(HexTilingSection section, const std::string& path);
// A static mesh detail texture's RaHexDetail (16 floats, see RaMaterialParameters::hexDetail): [0]
// the mode (0 not listed, 1 the whole sheet, 2 its strips), [1] the whole sheet's shift steps a tile
// (0 any shift), [4..11] up to four (from, to) V ranges, unused ones (1, 0), [12..15] their steps.
void hexTilingStatic(const std::string& path, float out[16]);

} // namespace geom
} // namespace hfe
} // namespace dice

#endif

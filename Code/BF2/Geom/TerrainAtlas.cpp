// [RENDER] Terrain texture atlases (render milestone 10, task 2); see TerrainAtlas.h.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/PluginAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "../RendDX9/RendDX9Base.h"
#include "../RendDX9/Texture.h"
#include "TerrainAtlas.h"
#include "HexTilingList.h"
#include "Terrain.h"
#include "QuadPatch.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
namespace geom {

namespace {

TerrainAtlases g_atlases = { std::vector<IDirect3DTexture9*>(), std::vector<TerrainAtlasEntry>(),
                             std::vector<TerrainAtlasEntry>(), std::vector<unsigned char>(), 0, 0, 0 };
// Engine texture records wrapping the detail atlases: effect parameters take a texture as one.
std::vector<dx9rend::EngineTexture*> g_detailValues;
dx9rend::EngineTexture g_lightmapValue;   // the lightmap atlas as an effect texture value
bool g_built = false;
Terrain* g_pending = 0;   // loaded, waiting for its textures (the texture manager's post-load sweep)

// A copy unit: 4x4 texels for the block-compressed formats, one texel otherwise.
struct BlockFormat
{
    int dim;
    int bytes;
};

bool blockFormat(D3DFORMAT format, BlockFormat& out)
{
    switch (format)
    {
    case D3DFMT_DXT1:                                    out.dim = 4; out.bytes = 8;  return true;
    case D3DFMT_DXT2: case D3DFMT_DXT3:
    case D3DFMT_DXT4: case D3DFMT_DXT5:                  out.dim = 4; out.bytes = 16; return true;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:          out.dim = 1; out.bytes = 4;  return true;
    case D3DFMT_R5G6B5: case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_A8L8:                                    out.dim = 1; out.bytes = 2;  return true;
    case D3DFMT_L8: case D3DFMT_A8:                      out.dim = 1; out.bytes = 1;  return true;
    default:                                             return false;
    }
}

std::string formatName(D3DFORMAT format)
{
    const unsigned int f = static_cast<unsigned int>(format);
    char text[16];
    if (f > 0xFFu)
        ::sprintf_s(text, "%c%c%c%c", static_cast<char>(f & 0xFFu), static_cast<char>((f >> 8) & 0xFFu),
                    static_cast<char>((f >> 16) & 0xFFu), static_cast<char>(f >> 24));
    else
        ::sprintf_s(text, "fmt%u", f);
    return text;
}

IDirect3DTexture9* d3dTexture(const void* engineTexture)
{
    if (!engineTexture)
        return 0;
    IDirect3DBaseTexture9* const base = static_cast<const dx9rend::EngineTexture*>(engineTexture)->baseTexture;
    if (!base || base->GetType() != D3DRTYPE_TEXTURE)
        return 0;
    return static_cast<IDirect3DTexture9*>(base);
}

// A locked mip level of a source texture, in copy units.
struct LockedLevel
{
    const unsigned char* bits;
    int                  pitch;
    int                  blocksX;
    int                  blocksY;
};

bool lockSource(IDirect3DTexture9* texture, unsigned int level, const BlockFormat& block, LockedLevel& out)
{
    D3DSURFACE_DESC desc = {};
    D3DLOCKED_RECT rect;
    if (FAILED(texture->GetLevelDesc(level, &desc))
        || FAILED(texture->LockRect(level, &rect, 0, D3DLOCK_READONLY)))
        return false;
    out.bits = static_cast<const unsigned char*>(rect.pBits);
    out.pitch = rect.Pitch;
    out.blocksX = (static_cast<int>(desc.Width) + block.dim - 1) / block.dim;
    out.blocksY = (static_cast<int>(desc.Height) + block.dim - 1) / block.dim;
    return true;
}

// Fills a cell of a locked destination level from a source level. `wrapShift`: the cell holds the
// source repeated, starting half a tile in (detail entries); otherwise the source is scaled to the
// cell by nearest block (lightmaps).
void copyCell(unsigned char* dst, int dstPitch, int cellX, int cellY, int cellW, int cellH,
              const LockedLevel& src, const BlockFormat& block, bool wrapShift)
{
    for (int j = 0; j != cellH; ++j)
    {
        const int sy = wrapShift ? (j + src.blocksY / 2) % src.blocksY : j * src.blocksY / cellH;
        const unsigned char* const srcRow = src.bits + sy * src.pitch;
        unsigned char* const dstRow = dst + (cellY + j) * dstPitch + cellX * block.bytes;
        if (wrapShift)
        {
            for (int i = 0; i != cellW; ++i)
                ::memcpy(dstRow + i * block.bytes,
                         srcRow + ((i + src.blocksX / 2) % src.blocksX) * block.bytes, block.bytes);
        }
        else if (src.blocksX == cellW)
        {
            ::memcpy(dstRow, srcRow, cellW * block.bytes);
        }
        else
        {
            for (int i = 0; i != cellW; ++i)
                ::memcpy(dstRow + i * block.bytes, srcRow + (i * src.blocksX / cellW) * block.bytes,
                         block.bytes);
        }
    }
}

IDirect3DDevice9* device() { return static_cast<IDirect3DDevice9*>(dx9rend::renderDevice()); }

bool fitsDevice(unsigned int width, unsigned int height)
{
    D3DCAPS9 caps;
    if (FAILED(device()->GetDeviceCaps(&caps)))
        return false;
    return width <= caps.MaxTextureWidth && height <= caps.MaxTextureHeight;
}

// One detail atlas: its entries share format and size.
struct DetailGroup
{
    D3DFORMAT                       format;
    unsigned int                    width;
    unsigned int                    height;
    unsigned int                    levels;
    std::vector<IDirect3DTexture9*> sources;
};

bool buildDetailAtlas(DetailGroup& group, IDirect3DTexture9*& out, int& columns)
{
    BlockFormat block;
    if (!blockFormat(group.format, block))
    {
        ::renddx9abi::logf("[terrain.atlas] detail format %s not supported", formatName(group.format).c_str());
        return false;
    }
    // The last level keeps a tile of two copy units, so the half-tile shift stays whole.
    unsigned int levels = group.levels;
    while (levels > 1
           && ((group.width >> (levels - 1)) < 2u * block.dim || (group.height >> (levels - 1)) < 2u * block.dim))
        --levels;
    group.levels = levels;

    const int count = static_cast<int>(group.sources.size());
    columns = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const int rows = (count + columns - 1) / columns;
    const unsigned int atlasW = 2u * group.width * columns;
    const unsigned int atlasH = 2u * group.height * rows;
    if (!fitsDevice(atlasW, atlasH)
        || FAILED(device()->CreateTexture(atlasW, atlasH, levels, 0, group.format, D3DPOOL_MANAGED, &out, 0)))
    {
        ::renddx9abi::logf("[terrain.atlas] could not create a %ux%u %s detail atlas", atlasW, atlasH,
                           formatName(group.format).c_str());
        out = 0;
        return false;
    }

    for (unsigned int level = 0; level != levels; ++level)
    {
        D3DLOCKED_RECT dst;
        if (FAILED(out->LockRect(level, &dst, 0, 0)))
            continue;
        const int cellW = static_cast<int>(2u * (group.width >> level)) / block.dim;
        const int cellH = static_cast<int>(2u * (group.height >> level)) / block.dim;
        for (int n = 0; n != count; ++n)
        {
            LockedLevel src;
            if (!lockSource(group.sources[n], level, block, src))
            {
                ::renddx9abi::logf("[terrain.atlas] detail source %d level %u could not be read", n, level);
                continue;
            }
            copyCell(static_cast<unsigned char*>(dst.pBits), dst.Pitch, (n % columns) * cellW,
                     (n / columns) * cellH, cellW, cellH, src, block, true);
            group.sources[n]->UnlockRect(level);
        }
        out->UnlockRect(level);
    }
    return true;
}

void buildDetailAtlases(Terrain* terrain)
{
    const int records = static_cast<int>(terrain->m_detailTextures.size());
    const TerrainAtlasEntry none = { -1, 0.0f, 0.0f, 0.0f, 0.0f };
    g_atlases.top.assign(records, none);
    g_atlases.side.assign(records, none);
    g_atlases.grid.assign(records, 0);
    for (int r = 0; r != records; ++r)
    {
        const TerrainDetailTexture* const record = terrain->m_detailTextures[r];
        const dx9rend::EngineTexture* const texture =
            record ? static_cast<const dx9rend::EngineTexture*>(record->texture) : 0;
        if (texture && hexTilingListed(kHexTerrainGrid, texture->name))
            g_atlases.grid[r] = 1;
    }

    // Distinct source textures, grouped by format and size; slot[k] = (group, index in group).
    std::vector<DetailGroup> groups;
    std::vector<IDirect3DTexture9*> textures;
    std::vector<std::pair<int, int> > slot;
    std::vector<int> topSlot(records, -1), sideSlot(records, -1);
    for (int r = 0; r != records; ++r)
    {
        TerrainDetailTexture* const record = terrain->m_detailTextures[r];
        if (!record)
            continue;
        for (int which = 0; which != 2; ++which)
        {
            IDirect3DTexture9* const texture = d3dTexture(which ? record->sideTexture : record->texture);
            if (!texture)
                continue;
            int k = 0;
            while (k != static_cast<int>(textures.size()) && textures[k] != texture)
                ++k;
            if (k == static_cast<int>(textures.size()))
            {
                D3DSURFACE_DESC desc = {};
                texture->GetLevelDesc(0, &desc);
                const unsigned int levels = texture->GetLevelCount();
                int g = 0;
                while (g != static_cast<int>(groups.size())
                       && (groups[g].format != desc.Format || groups[g].width != desc.Width
                           || groups[g].height != desc.Height))
                    ++g;
                if (g == static_cast<int>(groups.size()))
                {
                    DetailGroup group;
                    group.format = desc.Format;
                    group.width = desc.Width;
                    group.height = desc.Height;
                    group.levels = levels;
                    groups.push_back(group);
                }
                if (levels < groups[g].levels)
                    groups[g].levels = levels;
                textures.push_back(texture);
                slot.push_back(std::make_pair(g, static_cast<int>(groups[g].sources.size())));
                groups[g].sources.push_back(texture);
            }
            (which ? sideSlot : topSlot)[r] = k;
        }
    }

    std::vector<int> atlasOf(groups.size(), -1), columnsOf(groups.size(), 0);
    for (std::size_t g = 0; g != groups.size(); ++g)
    {
        IDirect3DTexture9* atlas = 0;
        if (!buildDetailAtlas(groups[g], atlas, columnsOf[g]))
            continue;
        atlasOf[g] = static_cast<int>(g_atlases.detail.size());
        g_atlases.detail.push_back(atlas);
        dx9rend::EngineTexture* const value = new dx9rend::EngineTexture();
        value->baseTexture = atlas;
        g_detailValues.push_back(value);
        D3DSURFACE_DESC desc = {};
        atlas->GetLevelDesc(0, &desc);
        ::renddx9abi::logf("[terrain.atlas] detail atlas %d: %s %ux%u m%u, %u entries of %ux%u",
                           atlasOf[g], formatName(groups[g].format).c_str(), desc.Width, desc.Height,
                           groups[g].levels, static_cast<unsigned int>(groups[g].sources.size()),
                           groups[g].width, groups[g].height);
    }

    for (int r = 0; r != records; ++r)
    {
        for (int which = 0; which != 2; ++which)
        {
            const int k = (which ? sideSlot : topSlot)[r];
            if (k < 0 || atlasOf[slot[k].first] < 0)
                continue;
            const DetailGroup& group = groups[slot[k].first];
            const int columns = columnsOf[slot[k].first];
            const int rows = (static_cast<int>(group.sources.size()) + columns - 1) / columns;
            const int n = slot[k].second;
            TerrainAtlasEntry& entry = (which ? g_atlases.side : g_atlases.top)[r];
            entry.atlas = atlasOf[slot[k].first];
            entry.scaleU = 0.5f / columns;
            entry.scaleV = 0.5f / rows;
            entry.offsetU = (n % columns + 0.25f) / columns;
            entry.offsetV = (n / columns + 0.25f) / rows;
        }
    }
}

void buildLightmapAtlas(Terrain* terrain)
{
    const int count = terrain->m_patchCount;
    const int total = count * count;
    if (!terrain->m_patches || count <= 0)
        return;

    // The cell is the largest patch lightmap; the format is the first one's.
    unsigned int cell = 0;
    D3DFORMAT format = D3DFMT_UNKNOWN;
    for (int p = 0; p != total; ++p)
    {
        IDirect3DTexture9* const texture = terrain->m_patches[p] ? d3dTexture(terrain->m_patches[p]->lightmap) : 0;
        if (!texture)
            continue;
        D3DSURFACE_DESC desc = {};
        texture->GetLevelDesc(0, &desc);
        if (format == D3DFMT_UNKNOWN)
            format = desc.Format;
        if (desc.Width > cell)
            cell = desc.Width;
    }
    BlockFormat block;
    if (!cell || !blockFormat(format, block))
    {
        ::renddx9abi::logf("[terrain.atlas] no lightmap atlas (cell %u, format %s)", cell, formatName(format).c_str());
        return;
    }
    unsigned int levels = 1;
    while ((cell >> levels) >= static_cast<unsigned int>(block.dim))
        ++levels;
    const unsigned int size = cell * count;
    IDirect3DTexture9* atlas = 0;
    if (!fitsDevice(size, size)
        || FAILED(device()->CreateTexture(size, size, levels, 0, format, D3DPOOL_MANAGED, &atlas, 0)))
    {
        ::renddx9abi::logf("[terrain.atlas] could not create a %ux%u %s lightmap atlas", size, size,
                           formatName(format).c_str());
        return;
    }

    int scaled = 0, missing = 0, mismatched = 0;
    for (unsigned int level = 0; level != levels; ++level)
    {
        D3DLOCKED_RECT dst;
        if (FAILED(atlas->LockRect(level, &dst, 0, 0)))
            continue;
        const int cellBlocks = static_cast<int>(cell >> level) / block.dim;
        for (int p = 0; p != total; ++p)
        {
            QuadPatch* const patch = terrain->m_patches[p];
            IDirect3DTexture9* const texture = patch ? d3dTexture(patch->lightmap) : 0;
            // The cell by the patch's grid position (centred on 0), so world position maps to it.
            int x = p % count, z = p / count;
            if (patch && patch->gridX + count / 2 >= 0 && patch->gridX + count / 2 < count
                && patch->gridZ + count / 2 >= 0 && patch->gridZ + count / 2 < count)
            {
                x = patch->gridX + count / 2;
                z = patch->gridZ + count / 2;
            }
            unsigned char* const base = static_cast<unsigned char*>(dst.pBits);
            D3DSURFACE_DESC desc = {};
            if (texture)
                texture->GetLevelDesc(0, &desc);
            if (!texture || desc.Format != format)
            {
                // No lightmap: zero blocks (black for every supported format).
                for (int j = 0; j != cellBlocks; ++j)
                    ::memset(base + (z * cellBlocks + j) * dst.Pitch + x * cellBlocks * block.bytes, 0,
                             cellBlocks * block.bytes);
                if (level == 0)
                    ++(texture ? mismatched : missing);
                continue;
            }
            // The source level closest to the cell's size from above, else the source's smallest.
            unsigned int s = 0;
            const unsigned int want = cell >> level;
            while (s + 1 < texture->GetLevelCount() && (desc.Width >> (s + 1)) >= want)
                ++s;
            if (level == 0 && desc.Width != cell)
                ++scaled;
            LockedLevel src;
            if (!lockSource(texture, s, block, src))
                continue;
            copyCell(base, dst.Pitch, x * cellBlocks, z * cellBlocks, cellBlocks, cellBlocks, src, block, false);
            texture->UnlockRect(s);
        }
        atlas->UnlockRect(level);
    }
    g_atlases.lightmap = atlas;
    g_atlases.lightmapCell = static_cast<int>(cell);
    g_atlases.lightmapGrid[0] = 1.0f / (static_cast<float>(terrain->m_patchSize) * terrain->m_primaryWorldScale.x);
    g_atlases.lightmapGrid[1] = 1.0f / (static_cast<float>(terrain->m_patchSize) * terrain->m_primaryWorldScale.z);
    g_atlases.lightmapGrid[2] = static_cast<float>(count);
    g_atlases.lightmapGrid[3] = static_cast<float>(cell);
    g_lightmapValue.baseTexture = atlas;
    ::renddx9abi::logf("[terrain.atlas] lightmap atlas: %s %ux%u m%u, %dx%d cells of %u (%d scaled up, "
                       "%d missing, %d of another format)",
                       formatName(format).c_str(), size, size, levels, count, count, cell, scaled, missing,
                       mismatched);
}

} // namespace

void terrainAtlasRelease()
{
    for (std::size_t i = 0; i != g_atlases.detail.size(); ++i)
        if (g_atlases.detail[i])
            g_atlases.detail[i]->Release();
    g_atlases.detail.clear();
    for (std::size_t i = 0; i != g_detailValues.size(); ++i)
        delete g_detailValues[i];
    g_detailValues.clear();
    g_atlases.top.clear();
    g_atlases.side.clear();
    g_atlases.grid.clear();
    if (g_atlases.lightmap)
        g_atlases.lightmap->Release();
    g_atlases.lightmap = 0;
    g_atlases.lightmapCell = 0;
    g_lightmapValue.baseTexture = 0;
    g_atlases.patchCount = 0;
    g_built = false;
    g_pending = 0;
}

void terrainAtlasLoad(Terrain* terrain)
{
    terrainAtlasRelease();
    hexTilingListLoad();   // [RENDER] render milestone 19: the list as it is now, for this level
    g_pending = terrain;
}

void terrainAtlasUpdate()
{
    Terrain* const terrain = g_pending;
    if (!terrain || !device())
        return;
    g_pending = 0;
    LARGE_INTEGER start, end, frequency;
    ::QueryPerformanceCounter(&start);
    g_atlases.patchCount = terrain->m_patchCount;
    buildDetailAtlases(terrain);
    buildLightmapAtlas(terrain);
    g_built = true;
    ::QueryPerformanceCounter(&end);
    ::QueryPerformanceFrequency(&frequency);
    ::renddx9abi::logf("[terrain.atlas] built in %.1f ms",
                       1000.0 * static_cast<double>(end.QuadPart - start.QuadPart) / frequency.QuadPart);
}

const TerrainAtlases* terrainAtlases()
{
    return g_built ? &g_atlases : 0;
}

const void* terrainAtlasDetailValue(int index)
{
    if (!g_built || index < 0 || index >= static_cast<int>(g_detailValues.size()))
        return 0;
    return g_detailValues[index];
}

const void* terrainAtlasLightmapValue()
{
    return (g_built && g_atlases.lightmap) ? &g_lightmapValue : 0;
}

const void* terrainRoadLight(float grid[4], float giDirect[4])
{
    if (!g_built || !g_atlases.lightmap || !g_Terrain)
        return 0;
    for (int i = 0; i != 4; ++i)
        grid[i] = g_atlases.lightmapGrid[i];
    Vec3 gi;
    static_cast<Terrain*>(g_Terrain)->getGIColorRaw(&gi);
    giDirect[0] = gi.x;
    giDirect[1] = gi.y;
    giDirect[2] = gi.z;
    giDirect[3] = 1.0f;
    return &g_lightmapValue;
}

void terrainAtlasDump(const std::string& dir)
{
    if (!g_built)
    {
        ::renddx9abi::logf("[terrain.atlas] nothing to dump: no terrain loaded");
        return;
    }
    ::CreateDirectoryA(dir.c_str(), 0);
    char path[MAX_PATH];
    for (std::size_t i = 0; i != g_atlases.detail.size(); ++i)
    {
        ::sprintf_s(path, "%s/detail%u.dds", dir.c_str(), static_cast<unsigned int>(i));
        const HRESULT hr = D3DXSaveTextureToFileA(path, D3DXIFF_DDS, g_atlases.detail[i], 0);
        ::renddx9abi::logf("[terrain.atlas] %s %s", path, SUCCEEDED(hr) ? "written" : "FAILED");
    }
    if (g_atlases.lightmap)
    {
        ::sprintf_s(path, "%s/lightmap.dds", dir.c_str());
        const HRESULT hr = D3DXSaveTextureToFileA(path, D3DXIFF_DDS, g_atlases.lightmap, 0);
        ::renddx9abi::logf("[terrain.atlas] %s %s", path, SUCCEEDED(hr) ? "written" : "FAILED");
    }
    for (std::size_t r = 0; r != g_atlases.top.size(); ++r)
    {
        const TerrainAtlasEntry& t = g_atlases.top[r];
        const TerrainAtlasEntry& s = g_atlases.side[r];
        ::renddx9abi::logf("[terrain.atlas] record %u: top atlas %d scale %g %g offset %g %g; "
                           "side atlas %d scale %g %g offset %g %g",
                           static_cast<unsigned int>(r), t.atlas, t.scaleU, t.scaleV, t.offsetU, t.offsetV,
                           s.atlas, s.scaleU, s.scaleV, s.offsetU, s.offsetV);
    }
}

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

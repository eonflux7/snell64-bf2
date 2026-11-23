// [RENDER] The far ground (render milestone 17); see FarGround.h.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/PluginAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "../RendDX9/RendDX9Base.h"
#include "../RendDX9/ShaderParameter.h"
#include "../RendDX9/Texture.h"
#include "../RendMisc/IRenderView.h"
#include "../Physics/HeightmapCluster.h"
#include "FarGround.h"
#include "Terrain.h"
#include "TerrainAtlas.h"
#include "QuadPatch.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

namespace dx9rend {
void* bufferManagerCreateVertexBuffer(unsigned int count, unsigned int stride,
                                      unsigned int usage, const char* name);
void* vertexBufferLock(void* wrapper, unsigned int offsetVerts, unsigned int countVerts,
                       unsigned int flags);
bool  vertexBufferUnlock(void* wrapper);
void  vertexBufferRelease(void* wrapper);
void  vertexBufferBindStream(void* wrapper, unsigned int stream);
void* bufferManagerCreateIndexBuffer(unsigned int count, unsigned int usage, unsigned int format);
void* indexBufferLock(void* wrapper, unsigned int offsetIndices, unsigned int countIndices,
                      unsigned int flags);
bool  indexBufferUnlock(void* wrapper);
void  indexBufferRelease(void* wrapper);
void  indexBufferBind(void* wrapper);
void  vertexDeclarationBind(void* wrapper);
void  rendererDrawIndexedPrimitive(unsigned int primCount, unsigned int type, int baseVertex,
                                   unsigned int minIndex, unsigned int numVertices,
                                   unsigned int startIndex);

// renderer.farGround: 0 the retail surrounding ring; 1 the far ground on levels with a bake, the
// ring elsewhere; 2 the far ground everywhere (without a bake, its placeholder heights).
int g_farGround = 1;
int& rendererFarGround() { return g_farGround; }
// renderer.farGroundScaled: the bake's heights about its pivot times its relief ratio (1), or real (0).
bool g_farGroundScaled = false;
bool& rendererFarGroundScaled() { return g_farGroundScaled; }
} // namespace dx9rend

namespace geom {

namespace {

// The mesh: kLevels nested levels of kCells x kCells cells around the camera, cell kCell0 * 2^level.
// Level 0 is whole; each coarser one leaves out its middle (kHole cells from its centre on each
// side), which the finer level covers with one coarse cell to spare whatever their snapping.
const int   kLevels = 9;
const int   kCells = 64;
const int   kHole = kCells / 4 - 1;
const float kCell0 = 4.0f;
const int   kSide = kCells + 1;                                    // vertices a side
const float kMorphStart = 16.0f;                                   // cells from the camera
const float kMorphRange = 10.0f;                                   // complete 26 cells out
const float kEarthRadius = 6371000.0f;
const unsigned int kColorTexels = 512;                             // the map's colour, texels a side (at most)

// The heights past the map.
const int   kFarNodes = 1024;                                      // the coarse heights, nodes a side
const float kFarReach = 34000.0f;                                  // m past the map's half side
const float kFineBlend = 1500.0f;                                  // m inside the fine square's edge: fine to coarse
const float kBaseFall = 6000.0f;                                   // m past known ground: at the base height
const float kBakeBlend = 3000.0f;                                  // m past known ground: on the bake
const float kEdgeReach = 256.0f;                                   // m past the map: the edge heights at its cell
const float kSkirtDepth = 30.0f;                                   // m: the strip under the map's edge
const int   kSkirtLevel = 9;                                       // its slot in RaFarGroundLevels (origin 0, cell 1)
const float kSeaNear = 10.0f;                                      // m: a sea level under the map's lowest by more is none
const float kFillerDepth = 5.0f;                                   // m under the map's lowest: ring filler
const float kFlatSector = 1.0f;                                    // m: a sector with less relief is a placeholder
const float kDetailFadeEnd = 1500.0f;                              // m from the camera: the detail texture gone
const float kDetailFadeRange = 1000.0f;


struct Params
{
    void* levels;    // RaFarGroundLevels (float4[8]: origin x, z, cell, morphs)
    void* constants; // RaFarGround (float4[8], layout in the shader)
    void* height;    // RaFarGroundHeight (texture, fine)
    void* heightFar; // RaFarGroundHeightFar (texture, coarse)
    void* heightEdge; // RaFarGroundHeightEdge (texture, the map's cell around its edge)
    void* color;     // RaFarGroundColor (texture)
};

// A square grid of nodes: node (i, j) at (origin + i * cell, origin + j * cell).
struct Grid
{
    int   n;
    float origin;
    float cell;
    float x(int i) const { return origin + static_cast<float>(i) * cell; }
};

struct State
{
    // The ring's heights as the slots load, over the ring's square; the slot each node came from.
    float              ringSpan;
    float              ringCell;
    int                ringNodes;
    std::vector<float> ringSum;
    std::vector<float> ringCount;
    std::vector<signed char> ringSlot;
    float              slotMin[8];
    float              slotMax[8];
    unsigned int       slotHash[8];
    bool               slotSeen[8];

    Terrain*           pending;
    bool               built;
    bool               baked;     // the level has a bake file
    IDirect3DTexture9* heightTexture;
    IDirect3DTexture9* heightFarTexture;
    IDirect3DTexture9* heightEdgeTexture;
    IDirect3DTexture9* colorTexture;
    dx9rend::EngineTexture heightValue;
    dx9rend::EngineTexture heightFarValue;
    dx9rend::EngineTexture heightEdgeValue;
    dx9rend::EngineTexture colorValue;
    void*              vertexBuffer;
    void*              indexBuffer;
    void*              declaration;
    unsigned int       vertexCount;
    unsigned int       primCount;
    Params             params;
    float              constants[8][4];

    // The coarse heights on the CPU (the sea test) and the sea level, when it is a sea.
    Grid               coarse;
    std::vector<float> farHeight;
    bool               seaValid;
    float              seaLevel;

    // What composeHeights needs to rebuild the heights (the bake's variant changing): the known
    // heights pulled out on both grids, each node's distance past known ground (0 on it).
    Grid               fine;
    std::vector<float> fineE;
    std::vector<float> fineDist;
    std::vector<float> farE;
    std::vector<float> farDist;
    Grid               edge;      // the map's square plus kEdgeReach, at the map's cell
    std::vector<float> edgeMap;   // its nodes on the map: the map's heights; NaN past it
    float              base;
    bool               seaNear;   // the level's sea lies near the map's heights
    int                variant;   // the bake's variant the heights were composed with (-1 none)

    // The level's bake (<mod>/settings/eon64-farground/<level>.bin): heights in the level's datum
    // on a square grid, and the scaled variant's ratio and pivot.
    Grid               bakeGrid;
    std::vector<float> bake;
    float              bakeRatio;
    float              bakePivot;
    bool               bakeSea;   // baked with the real sea on the level's (coastal levels)
};

State g;

void releaseBuilt();

IDirect3DDevice9* device() { return static_cast<IDirect3DDevice9*>(dx9rend::renderDevice()); }

IDirect3DTexture9* d3dTexture(const void* engineTexture)
{
    if (!engineTexture)
        return 0;
    IDirect3DBaseTexture9* const base = static_cast<const dx9rend::EngineTexture*>(engineTexture)->baseTexture;
    if (!base || base->GetType() != D3DRTYPE_TEXTURE)
        return 0;
    return static_cast<IDirect3DTexture9*>(base);
}

inline dx9rend::ShaderParameter* param(void* handle) { return static_cast<dx9rend::ShaderParameter*>(handle); }

world::IHeightmapCluster* heightmapCluster()
{
    IBase* const found = ::dice::hfe::classManager->getSingleton(::renddx9abi::HostString("HeightmapCluster"));
    return static_cast<world::IHeightmapCluster*>(static_cast<void*>(found));
}

void releaseParam(void*& handle)
{
    if (handle)
        param(handle)->release();
    handle = 0;
}

void releaseTexture(IDirect3DTexture9*& texture, dx9rend::EngineTexture& value)
{
    if (texture)
        texture->Release();
    texture = 0;
    value.baseTexture = 0;
}

void clearRing()
{
    g.ringSpan = 0.0f;
    g.ringCell = 0.0f;
    g.ringNodes = 0;
    std::vector<float>().swap(g.ringSum);
    std::vector<float>().swap(g.ringCount);
    std::vector<signed char>().swap(g.ringSlot);
    for (int s = 0; s != 8; ++s)
    {
        g.slotMin[s] = FLT_MAX;
        g.slotMax[s] = -FLT_MAX;
        g.slotHash[s] = 0u;
        g.slotSeen[s] = false;
    }
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float smoothstep(float a, float b, float v)
{
    const float t = clamp01((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

inline float srgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c)
{
    c = clamp01(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// Fills the grid's empty nodes by pull-push: each coarser level averages the nodes under it, then
// from the coarsest down an empty node takes the next coarser level bilinearly.
void pullPush(std::vector<float>& value, std::vector<float>& weight, int n)
{
    if (n <= 1)
        return;
    const int c = (n + 1) / 2;
    std::vector<float> coarse(static_cast<std::size_t>(c) * c, 0.0f), coarseWeight(coarse.size(), 0.0f);
    for (int j = 0; j != c; ++j)
        for (int i = 0; i != c; ++i)
        {
            float sum = 0.0f, w = 0.0f;
            for (int b = 0; b != 2; ++b)
                for (int a = 0; a != 2; ++a)
                {
                    const int x = 2 * i + a, z = 2 * j + b;
                    if (x >= n || z >= n)
                        continue;
                    const std::size_t k = static_cast<std::size_t>(z) * n + x;
                    sum += value[k] * weight[k];
                    w += weight[k];
                }
            const std::size_t k = static_cast<std::size_t>(j) * c + i;
            coarse[k] = w > 0.0f ? sum / w : 0.0f;
            coarseWeight[k] = w > 0.0f ? 1.0f : 0.0f;
        }
    pullPush(coarse, coarseWeight, c);
    for (int j = 0; j != n; ++j)
        for (int i = 0; i != n; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * n + i;
            if (weight[k] > 0.0f)
                continue;
            // Fine node i sits at coarse coordinate i / 2 - 0.25 (centres of the 2x2 blocks).
            const float fx = (std::min)((std::max)(0.5f * i - 0.25f, 0.0f), static_cast<float>(c - 1));
            const float fz = (std::min)((std::max)(0.5f * j - 0.25f, 0.0f), static_cast<float>(c - 1));
            const int x0 = static_cast<int>(fx), z0 = static_cast<int>(fz);
            const int x1 = (std::min)(x0 + 1, c - 1), z1 = (std::min)(z0 + 1, c - 1);
            const float tx = fx - x0, tz = fz - z0;
            const float a = coarse[z0 * c + x0] + (coarse[z0 * c + x1] - coarse[z0 * c + x0]) * tx;
            const float b = coarse[z1 * c + x0] + (coarse[z1 * c + x1] - coarse[z1 * c + x0]) * tx;
            value[k] = a + (b - a) * tz;
            weight[k] = 1.0f;
        }
}

// Bilinear lookup in a grid, clamped to its extent.
float sampleGrid(const std::vector<float>& value, const Grid& grid, float x, float z)
{
    const float last = static_cast<float>(grid.n - 1);
    const float fx = (std::min)((std::max)((x - grid.origin) / grid.cell, 0.0f), last);
    const float fz = (std::min)((std::max)((z - grid.origin) / grid.cell, 0.0f), last);
    const int x0 = (std::min)(static_cast<int>(fx), grid.n - 2), z0 = (std::min)(static_cast<int>(fz), grid.n - 2);
    if (grid.n < 2)
        return value[0];
    const float tx = fx - x0, tz = fz - z0;
    const std::size_t k = static_cast<std::size_t>(z0) * grid.n + x0;
    const float a = value[k] + (value[k + 1] - value[k]) * tx;
    const float b = value[k + grid.n] + (value[k + grid.n + 1] - value[k + grid.n]) * tx;
    return a + (b - a) * tz;
}

// A box of known ground (world xz).
struct Box
{
    float x0, z0, x1, z1;
};

float distanceToKnown(const std::vector<Box>& known, float x, float z)
{
    float best = FLT_MAX;
    for (std::size_t b = 0; b != known.size(); ++b)
    {
        const float dx = (std::max)((std::max)(known[b].x0 - x, x - known[b].x1), 0.0f);
        const float dz = (std::max)((std::max)(known[b].z0 - z, z - known[b].z1), 0.0f);
        best = (std::min)(best, std::sqrt(dx * dx + dz * dz));
    }
    return best;
}

struct Field
{
    std::vector<Box> known;
    float            base;
    bool             seaValid;
    float            seaLevel;
};

bool uploadHeights(const std::vector<float>& value, int n, IDirect3DTexture9*& texture, dx9rend::EngineTexture& engineValue)
{
    if (!texture && FAILED(device()->CreateTexture(n, n, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &texture, 0)))
    {
        ::renddx9abi::logf("[farground] could not create the %dx%d height texture", n, n);
        texture = 0;
        return false;
    }
    D3DLOCKED_RECT rect;
    if (FAILED(texture->LockRect(0, &rect, 0, 0)))
        return false;
    for (int j = 0; j != n; ++j)
        std::memcpy(static_cast<unsigned char*>(rect.pBits) + j * rect.Pitch, &value[static_cast<std::size_t>(j) * n],
                    sizeof(float) * n);
    texture->UnlockRect(0);
    engineValue.baseTexture = texture;
    return true;
}

// A slot is data unless it is flat or byte-identical to another (placeholder files).
bool slotUsable(const Terrain* t, int s)
{
    if (!t->m_surrounding[s] || !g.slotSeen[s] || g.slotMax[s] - g.slotMin[s] < kFlatSector)
        return false;
    for (int o = 0; o != 8; ++o)
        if (o != s && g.slotSeen[o] && g.slotHash[o] == g.slotHash[s])
            return false;
    return true;
}

inline float percentile(std::vector<float>& v, float p)
{
    const std::size_t k = static_cast<std::size_t>(p * static_cast<float>(v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

// The bake's height at world (x, z), in the level's datum: real, or about its pivot times its
// ratio (renderer.farGroundScaled). Past the bake's square it holds its edge.
float bakeHeight(float x, float z, bool scaled)
{
    const float h = sampleGrid(g.bake, g.bakeGrid, x, z);
    return scaled ? g.bakePivot + (h - g.bakePivot) * g.bakeRatio : h;
}

// A node `d` metres past known ground, whose pulled-out known height is `e`: with a bake, the
// bake's height reached over kBakeBlend; without, the placeholder.
float pastKnown(float e, float d, float x, float z, bool scaled)
{
    if (!g.baked)
        return e + (g.base - e) * smoothstep(0.0f, kBaseFall, d);
    return e + (bakeHeight(x, z, scaled) - e) * smoothstep(0.0f, kBakeBlend, d);
}

// The heights from what buildHeights kept, for the bake's current variant; uploads both textures.
bool composeHeights()
{
    const bool scaled = dx9rend::rendererFarGroundScaled();
    const std::size_t farCount = g.farE.size(), fineCount = g.fineE.size();
    g.farHeight.assign(farCount, 0.0f);
    int farWater = 0;
    for (int j = 0; j != g.coarse.n; ++j)
        for (int i = 0; i != g.coarse.n; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * g.coarse.n + i;
            g.farHeight[k] = g.farDist[k] > 0.0f
                                 ? pastKnown(g.farE[k], g.farDist[k], g.coarse.x(i), g.coarse.x(j), scaled)
                                 : g.farE[k];
            farWater += g.seaNear && g.farHeight[k] < g.seaLevel;
        }
    std::vector<float> fineHeight(fineCount, 0.0f);
    for (int j = 0; j != g.fine.n; ++j)
        for (int i = 0; i != g.fine.n; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * g.fine.n + i;
            fineHeight[k] = g.fineDist[k] > 0.0f
                                ? pastKnown(g.fineE[k], g.fineDist[k], g.fine.x(i), g.fine.x(j), scaled)
                                : g.fineE[k];
        }
    // The edge heights: the map's own at its nodes (so the far ground's vertices on them meet the
    // map's), the fine heights past it.
    std::vector<float> edgeHeight(g.edgeMap);
    for (int j = 0; j != g.edge.n; ++j)
        for (int i = 0; i != g.edge.n; ++i)
        {
            float& h = edgeHeight[static_cast<std::size_t>(j) * g.edge.n + i];
            if (h != h)
                h = sampleGrid(fineHeight, g.fine, g.edge.x(i), g.edge.x(j));
        }
    if (!uploadHeights(fineHeight, g.fine.n, g.heightTexture, g.heightValue)
        || !uploadHeights(g.farHeight, g.coarse.n, g.heightFarTexture, g.heightFarValue)
        || !uploadHeights(edgeHeight, g.edge.n, g.heightEdgeTexture, g.heightEdgeValue))
        return false;
    // An inland bake has no water past known ground: the level's water (a river, Kunar Province's at
    // 36 m) would flood the real valleys under it. The CPU copy, the sea test's only input.
    if (g.baked && !g.bakeSea)
    {
        farWater = 0;
        for (std::size_t k = 0; k != farCount; ++k)
        {
            if (g.farDist[k] > 0.0f)
                g.farHeight[k] = (std::max)(g.farHeight[k], g.seaLevel);
            farWater += g.seaNear && g.farHeight[k] < g.seaLevel;
        }
    }
    g.seaValid = g.seaNear && farWater > 0;
    g.variant = scaled ? 1 : 0;
    return true;
}

// Reads the level's bake, if it has one. Layout (little-endian, 64-byte header): "EONFAR1\0",
// u32 version (1), u32 n (nodes a side), f32 origin (world x and z of node 0), f32 cell (m),
// f32 height scale and bias (height = bias + scale * value), f32 the scaled variant's ratio and
// pivot, f32 the sea level it was baked for, 5 reserved words; then n x n int16 rows of z, x
// across a row.
bool loadBake()
{
    std::string mod, level;
    dx9rend::readStringSettingDefault("GSModDirectory", "mods/bf2", mod);
    dx9rend::readStringSetting("GSLoadLevel", level);
    if (level.empty())
        return false;
    for (std::size_t i = 0; i != level.size(); ++i)
        level[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(level[i])));
    if (!mod.empty() && mod[mod.size() - 1] != '/' && mod[mod.size() - 1] != '\\')
        mod += '/';
    const std::string path = mod + "settings/eon64-farground/" + level + ".bin";
    std::FILE* in = std::fopen(path.c_str(), "rb");
    if (!in)
    {
        ::renddx9abi::logf("[farground] no bake for %s (%s)", level.c_str(), path.c_str());
        return false;
    }
    struct Header
    {
        char         magic[8];
        unsigned int version, n;
        float        origin, cell, scale, bias, ratio, pivot, sea;
        unsigned int reserved[5];
    } h;
    static_assert(sizeof(Header) == 64, "the bake's header is 64 bytes");
    bool ok = std::fread(&h, sizeof(h), 1, in) == 1 && std::memcmp(h.magic, "EONFAR1", 8) == 0 && h.version == 1
              && h.n >= 2 && h.n <= 8192 && h.cell > 0.0f;
    std::vector<short> raw;
    if (ok)
    {
        raw.resize(static_cast<std::size_t>(h.n) * h.n);
        ok = std::fread(&raw[0], sizeof(short), raw.size(), in) == raw.size();
    }
    std::fclose(in);
    if (!ok)
    {
        ::renddx9abi::logf("[farground] %s: not a bake (version 1)", path.c_str());
        return false;
    }
    g.bake.resize(raw.size());
    for (std::size_t k = 0; k != raw.size(); ++k)
        g.bake[k] = h.bias + h.scale * static_cast<float>(raw[k]);
    g.bakeGrid.n = static_cast<int>(h.n);
    g.bakeGrid.origin = h.origin;
    g.bakeGrid.cell = h.cell;
    g.bakeRatio = h.ratio;
    g.bakePivot = h.pivot;
    g.bakeSea = h.sea > -1.0e8f;
    ::renddx9abi::logf("[farground] bake %s: %ux%u of %.1f m from %.0f m; scaled variant x%.3f about %.1f m; "
                       "%s",
                       path.c_str(), h.n, h.n, h.cell, h.origin, h.ratio, h.pivot,
                       g.bakeSea ? "coastal, its sea on the level's" : "inland, no water past known ground");
    return true;
}

// How the bake meets the map (a check on its placement): over the map, the mean difference and
// the correlation of the bake's real heights with the map's, as placed and mirrored or turned.
void logBakeFit(world::IHeightmapCluster* cluster, float mapHalf)
{
    const float sx[4] = { 1.0f, -1.0f, 1.0f, -1.0f }, sz[4] = { 1.0f, 1.0f, -1.0f, -1.0f };
    double corr[4], meanDiff = 0.0;
    for (int o = 0; o != 4; ++o)
    {
        double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, n = 0;
        for (float z = -mapHalf + 32.0f; z < mapHalf; z += 64.0f)
            for (float x = -mapHalf + 32.0f; x < mapHalf; x += 64.0f)
            {
                const double a = cluster->getHeightInWorldCoords(x, z);
                const double b = bakeHeight(sx[o] * x, sz[o] * z, false);
                sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; n += 1;
            }
        const double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
        corr[o] = va > 0 && vb > 0 ? (sab - sa * sb / n) / std::sqrt(va * vb) : 0.0;
        if (o == 0)
            meanDiff = (sb - sa) / n;
    }
    ::renddx9abi::logf("[farground] bake over the map: %+.1f m against it on average; correlation %.2f as "
                       "placed (mirrored x %.2f, z %.2f, turned 180 %.2f)",
                       meanDiff, corr[0], corr[1], corr[2], corr[3]);
}

// The heights: known ground, the fine and coarse grids filled and uploaded.
bool buildHeights(Terrain* t, world::IHeightmapCluster* cluster, float mapHalf, float mapCell)
{
    Field f;
    f.known.push_back({ -mapHalf, -mapHalf, mapHalf, mapHalf });
    int usable = 0;
    for (int s = 0; s != 8; ++s)
        if (slotUsable(t, s))
        {
            const SurroundingPatch* const r = t->m_surrounding[s];
            f.known.push_back({ static_cast<float>(r->originX), static_cast<float>(r->originZ),
                                static_cast<float>(r->endX), static_cast<float>(r->endZ) });
            ++usable;
        }

    // The fine grid over the ring's square, at its cell (16 m a node without a ring).
    Grid fine;
    const float span = g.ringNodes > 1 ? g.ringSpan : 2.0f * mapHalf;
    fine.cell = g.ringNodes > 1 ? g.ringCell : span / 256.0f;
    fine.n = g.ringNodes > 1 ? g.ringNodes : static_cast<int>(std::floor(3.0f * span / fine.cell + 0.5f)) + 1;
    fine.origin = -1.5f * span;
    const std::size_t fineCount = static_cast<std::size_t>(fine.n) * fine.n;
    std::vector<float> fineValue(fineCount, 0.0f), fineWeight(fineCount, 0.0f);
    std::vector<float> mapHeights, knownHeights;
    std::vector<std::size_t> ringKnown;
    for (int j = 0; j != fine.n; ++j)
        for (int i = 0; i != fine.n; ++i)
        {
            const float x = fine.x(i), z = fine.x(j);
            const std::size_t k = static_cast<std::size_t>(j) * fine.n + i;
            if (std::fabs(x) <= mapHalf && std::fabs(z) <= mapHalf)
            {
                const float inside = mapHalf - 0.01f;   // the cluster's last row and column
                fineValue[k] = cluster->getHeightInWorldCoords((std::max)(-inside, (std::min)(x, inside)),
                                                               (std::max)(-inside, (std::min)(z, inside)));
                fineWeight[k] = 1.0f;
                mapHeights.push_back(fineValue[k]);
            }
            else if (g.ringNodes == fine.n && g.ringCount[k] > 0.0f && g.ringSlot[k] >= 0
                     && slotUsable(t, g.ringSlot[k]))
                ringKnown.push_back(k);
        }
    if (mapHeights.empty())
        return false;
    // Ring nodes under the sea and deeper than the map's own lowest point are filler (Fallujah
    // West's ring lies at 0 m under its river's 22.5 m), not sea bed.
    const float seaLevel = cluster->getSeaWaterLevel();
    const float mapLowest = *std::min_element(mapHeights.begin(), mapHeights.end());
    const float mapHighest = *std::max_element(mapHeights.begin(), mapHeights.end());
    int filler = 0;
    for (std::size_t r = 0; r != ringKnown.size(); ++r)
    {
        const std::size_t k = ringKnown[r];
        const float h = g.ringSum[k] / g.ringCount[k];
        if (h < seaLevel && h < mapLowest - kFillerDepth)
        {
            ++filler;
            continue;
        }
        fineValue[k] = h;
        fineWeight[k] = 1.0f;
        knownHeights.push_back(h);
    }
    knownHeights.insert(knownHeights.end(), mapHeights.begin(), mapHeights.end());

    // The base height: the median of the known ground (above the sea, when there is one).
    std::vector<float> scratch(knownHeights);
    f.base = percentile(scratch, 0.5f);

    // A sea only near the map's height range (levels set -50, -10000 and the like for none).
    f.seaLevel = seaLevel;
    f.seaValid = f.seaLevel >= mapLowest - kSeaNear && f.seaLevel <= mapHighest;
    if (f.seaValid)
    {
        // The base stays on land: the median of the known ground above the sea.
        std::vector<float> land;
        for (std::size_t k = 0; k != knownHeights.size(); ++k)
            if (knownHeights[k] > f.seaLevel)
                land.push_back(knownHeights[k]);
        if (!land.empty())
            f.base = (std::max)(percentile(land, 0.5f), f.seaLevel + 2.0f);
    }

    // The coarse grid: past the mesh's reach from anywhere on the map.
    Grid coarse;
    coarse.n = kFarNodes;
    coarse.origin = -(mapHalf + kFarReach);
    coarse.cell = 2.0f * (mapHalf + kFarReach) / static_cast<float>(coarse.n - 1);
    const std::size_t farCount = static_cast<std::size_t>(coarse.n) * coarse.n;

    // Pull the known heights out on both grids; the coarse one's known nodes from the fine grid.
    std::vector<float> fineE(fineValue);
    pullPush(fineE, fineWeight, fine.n);
    std::vector<float> farE(farCount, 0.0f), farWeight(farCount, 0.0f);
    std::vector<float> farDist(farCount, 0.0f);
    for (int j = 0; j != coarse.n; ++j)
        for (int i = 0; i != coarse.n; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * coarse.n + i;
            const float x = coarse.x(i), z = coarse.x(j);
            farDist[k] = distanceToKnown(f.known, x, z);
            if (farDist[k] > 0.0f)
                continue;
            farE[k] = sampleGrid(fineE, fine, x, z);
            farWeight[k] = 1.0f;
        }
    pullPush(farE, farWeight, coarse.n);

    // Kept for composeHeights: each node's distance past known ground.
    std::vector<float> fineDist(fineCount, 0.0f);
    for (int j = 0; j != fine.n; ++j)
        for (int i = 0; i != fine.n; ++i)
            fineDist[static_cast<std::size_t>(j) * fine.n + i] = distanceToKnown(f.known, fine.x(i), fine.x(j));
    g.fine = fine;
    g.coarse = coarse;
    g.edge.cell = mapCell;
    g.edge.n = static_cast<int>(std::floor(2.0f * (mapHalf + kEdgeReach) / mapCell + 0.5f)) + 1;
    g.edge.origin = -0.5f * static_cast<float>(g.edge.n - 1) * mapCell;
    g.edgeMap.assign(static_cast<std::size_t>(g.edge.n) * g.edge.n, std::numeric_limits<float>::quiet_NaN());
    for (int j = 0; j != g.edge.n; ++j)
        for (int i = 0; i != g.edge.n; ++i)
        {
            const float x = g.edge.x(i), z = g.edge.x(j);
            if (std::fabs(x) > mapHalf + 0.01f || std::fabs(z) > mapHalf + 0.01f)
                continue;
            const float inside = mapHalf - 0.01f;
            g.edgeMap[static_cast<std::size_t>(j) * g.edge.n + i] = cluster->getHeightInWorldCoords(
                (std::max)(-inside, (std::min)(x, inside)), (std::max)(-inside, (std::min)(z, inside)));
        }
    g.fineE.swap(fineE);
    g.fineDist.swap(fineDist);
    g.farE.swap(farE);
    g.farDist.swap(farDist);
    g.base = f.base;
    g.seaNear = f.seaValid;
    g.seaLevel = f.seaLevel;
    g.variant = -1;
    if (!composeHeights())
        return false;
    if (g.baked)
        logBakeFit(cluster, mapHalf);

    // [1]: fine uv = xz * x + y; z its half side (m); w 1 / the blend to the coarse grid.
    // [3].z the fine cell (m); [4]: coarse uv = xz * x + y.
    g.constants[1][0] = 1.0f / (static_cast<float>(fine.n) * fine.cell);
    g.constants[1][1] = (-fine.origin / fine.cell + 0.5f) / static_cast<float>(fine.n);
    g.constants[1][2] = -fine.origin;
    g.constants[1][3] = 1.0f / kFineBlend;
    g.constants[3][2] = fine.cell;
    g.constants[4][0] = 1.0f / (static_cast<float>(coarse.n) * coarse.cell);
    g.constants[4][1] = (-coarse.origin / coarse.cell + 0.5f) / static_cast<float>(coarse.n);
    // [4].zw: edge uv = xz * z + w; [3].w: the edge square's half side (m).
    g.constants[4][2] = 1.0f / (static_cast<float>(g.edge.n) * g.edge.cell);
    g.constants[4][3] = (-g.edge.origin / g.edge.cell + 0.5f) / static_cast<float>(g.edge.n);
    g.constants[3][3] = -g.edge.origin;

    ::renddx9abi::logf("[farground] heights: fine %dx%d of %.1f m, coarse %dx%d of %.1f m, edge %dx%d of %.1f m; "
                       "%d of 8 sectors data; base %.1f m; "
                       "sea %.1f m %s (map %.1f..%.1f m, %d ring nodes filler), %s past the map",
                       fine.n, fine.n, fine.cell, coarse.n, coarse.n, coarse.cell, g.edge.n, g.edge.n, g.edge.cell,
                       usable, f.base,
                       f.seaLevel, f.seaValid ? "is a sea" : "is none", mapLowest, mapHighest, filler,
                       g.seaValid ? "water" : "no water");
    return true;
}

// The mean of a texture's sub-rectangle (uv), read at the mip where it is about 16 texels wide:
// linear rgb of the texels as sRGB (`colour`), of pow(2 x texel, 2.2) (`detail`, the terrain's
// detail decode), or raw (weights).
enum MeanMode { kMeanRaw, kMeanDetail };

bool textureMean(IDirect3DTexture9* texture, float u0, float v0, float u1, float v1, MeanMode mode, float out[3])
{
    if (!texture)
        return false;
    D3DSURFACE_DESC desc;
    unsigned int level = 0;
    while (level + 1 < texture->GetLevelCount() && SUCCEEDED(texture->GetLevelDesc(level + 1, &desc))
           && (u1 - u0) * desc.Width >= 16.0f && (v1 - v0) * desc.Height >= 16.0f)
        ++level;
    if (FAILED(texture->GetLevelDesc(level, &desc)))
        return false;
    RECT rect = { static_cast<LONG>(u0 * desc.Width), static_cast<LONG>(v0 * desc.Height),
                  static_cast<LONG>(u1 * desc.Width + 0.5f), static_cast<LONG>(v1 * desc.Height + 0.5f) };
    rect.right = (std::max)(rect.right, rect.left + 1);
    rect.bottom = (std::max)(rect.bottom, rect.top + 1);
    // Compressed sources read whole blocks.
    if (desc.Format == D3DFMT_DXT1 || desc.Format == D3DFMT_DXT3 || desc.Format == D3DFMT_DXT5)
    {
        rect.left &= ~3L;
        rect.top &= ~3L;
        rect.right = (std::min)(static_cast<LONG>(desc.Width), (rect.right + 3L) & ~3L);
        rect.bottom = (std::min)(static_cast<LONG>(desc.Height), (rect.bottom + 3L) & ~3L);
    }
    const unsigned int w = static_cast<unsigned int>(rect.right - rect.left), h = static_cast<unsigned int>(rect.bottom - rect.top);
    IDirect3DSurface9* src = 0;
    IDirect3DSurface9* dst = 0;
    bool ok = false;
    if (SUCCEEDED(texture->GetSurfaceLevel(level, &src))
        && SUCCEEDED(device()->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &dst, 0))
        && SUCCEEDED(D3DXLoadSurfaceFromSurface(dst, 0, 0, src, 0, &rect, D3DX_FILTER_NONE, 0)))
    {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(dst->LockRect(&locked, 0, D3DLOCK_READONLY)))
        {
            double sum[3] = { 0.0, 0.0, 0.0 };
            for (unsigned int y = 0; y != h; ++y)
            {
                const unsigned char* row = static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch;
                for (unsigned int x = 0; x != w; ++x)
                    for (int c = 0; c != 3; ++c)
                    {
                        const float v = row[4 * x + 2 - c] / 255.0f;   // B, G, R in memory
                        sum[c] += mode == kMeanDetail ? std::pow(2.0f * v, 2.2f) : v;
                    }
            }
            dst->UnlockRect();
            for (int c = 0; c != 3; ++c)
                out[c] = static_cast<float>(sum[c] / (static_cast<double>(w) * h));
            ok = true;
        }
    }
    if (dst)
        dst->Release();
    if (src)
        src->Release();
    return ok;
}

// What the map's detail layers do to its colour: per detail record, the mean of its decoded
// texture (the terrain multiplies the colormap by it); per patch, those weighted by the patch's
// mean weights. Also the record that weighs most along the map's edge (the detail fade's).
struct DetailMeans
{
    std::vector<float> mean;    // 3 per record
    std::vector<char>  valid;
    std::vector<float> edgeWeight;
};

const float* detailMean(Terrain* t, const TerrainAtlases& atlases, DetailMeans& m, int index)
{
    if (index < 0 || index >= static_cast<int>(atlases.top.size()) || index >= static_cast<int>(m.valid.size()))
        return 0;
    if (m.valid[index] == 0)
    {
        m.valid[index] = -1;
        const TerrainAtlasEntry& e = atlases.top[index];
        if (e.atlas >= 0 && e.atlas < static_cast<int>(atlases.detail.size())
            && textureMean(atlases.detail[e.atlas], e.offsetU, e.offsetV, e.offsetU + e.scaleU, e.offsetV + e.scaleV,
                           kMeanDetail, &m.mean[3 * index]))
            m.valid[index] = 1;
    }
    return m.valid[index] > 0 ? &m.mean[3 * index] : 0;
}

// The patch's albedo factor (rgb, linear): the sum of its layers' weights times their means.
bool patchDetailFactor(Terrain* t, QuadPatch* patch, const TerrainAtlases& atlases, DetailMeans& m, bool edge,
                       float factor[3])
{
    float weights[2][3];
    bool haveWeights[2] = { false, false };
    for (int k = 0; k != 2; ++k)
        haveWeights[k] = textureMean(d3dTexture(patch->detailMap[k] ? patch->detailMap[k] : patch->detailMap[0]),
                                     0.0f, 0.0f, 1.0f, 1.0f, kMeanRaw, weights[k]);
    factor[0] = factor[1] = factor[2] = 0.0f;
    bool any = false;
    const int layers = static_cast<int>(patch->detailLayers.size());
    for (int layer = 0; layer < 6 && layer < layers; ++layer)
    {
        const QuadPatchDetailLayer* const detail = patch->detailLayers[layer];
        if (!detail)
            continue;
        const int index = detail->detailIndex;
        if (index < 0 || index / 3 > 1 || !haveWeights[index / 3])
            continue;
        const float* const mean = detailMean(t, atlases, m, index);
        if (!mean)
            continue;
        // The shader's dot(weights.xyz, selector): x, y, z the weight texture's r, g, b.
        const float* const w3 = weights[index / 3];
        const float w = w3[0] * detail->selectorX + w3[1] * detail->selectorY + w3[2] * detail->selectorZ;
        for (int c = 0; c != 3; ++c)
            factor[c] += w * mean[c];
        if (edge)
            m.edgeWeight[index] += w;
        any = true;
    }
    return any;
}

// The playable map's colour: each patch's colormap, at the smallest level that still covers its
// cell, scaled into the patch's cell (column = patch x, row = patch z, as the lightmap atlas), its
// detail layers' mean folded in. Also the detail fade's layer.
bool buildColor(Terrain* t, float mapHalf)
{
    const int count = t->m_patchCount;
    if (!t->m_patches || count <= 0)
        return false;
    unsigned int cellTexels = 4;
    while (cellTexels * 2 * count <= kColorTexels)
        cellTexels *= 2;
    const unsigned int size = cellTexels * count;
    IDirect3DTexture9* texture = 0;
    if (FAILED(device()->CreateTexture(size, size, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, 0)))
    {
        ::renddx9abi::logf("[farground] could not create the %ux%u colour texture", size, size);
        return false;
    }
    g.colorTexture = texture;
    g.colorValue.baseTexture = texture;
    const TerrainAtlases* const atlases = terrainAtlases();
    DetailMeans means;
    if (atlases)
    {
        means.mean.assign(3 * atlases->top.size(), 1.0f);
        means.valid.assign(atlases->top.size(), 0);
        means.edgeWeight.assign(atlases->top.size(), 0.0f);
    }

    // Each cell's patch and its detail factor (1 without atlases).
    std::vector<QuadPatch*> cells(static_cast<std::size_t>(count) * count, static_cast<QuadPatch*>(0));
    std::vector<float> factors(3 * cells.size(), 1.0f);
    for (int p = 0; p != count * count; ++p)
    {
        QuadPatch* const patch = t->m_patches[p];
        if (!patch || !d3dTexture(patch->colormap))
            continue;
        int x = p % count, z = p / count;
        if (patch->gridX + count / 2 >= 0 && patch->gridX + count / 2 < count
            && patch->gridZ + count / 2 >= 0 && patch->gridZ + count / 2 < count)
        {
            x = patch->gridX + count / 2;
            z = patch->gridZ + count / 2;
        }
        const std::size_t c = static_cast<std::size_t>(z) * count + x;
        cells[c] = patch;
        const bool edge = x == 0 || z == 0 || x == count - 1 || z == count - 1;
        float factor[3];
        if (atlases && patchDetailFactor(t, patch, *atlases, means, edge, factor))
            std::memcpy(&factors[3 * c], factor, sizeof(factor));
    }

    IDirect3DSurface9* dst = 0;
    texture->GetSurfaceLevel(0, &dst);
    int copied = 0;
    for (std::size_t c = 0; dst && c != cells.size(); ++c)
    {
        IDirect3DTexture9* const source = cells[c] ? d3dTexture(cells[c]->colormap) : 0;
        if (!source)
            continue;
        const unsigned int x = static_cast<unsigned int>(c % count), z = static_cast<unsigned int>(c / count);
        unsigned int level = 0;
        D3DSURFACE_DESC desc = {};
        while (level + 1 < source->GetLevelCount()
               && SUCCEEDED(source->GetLevelDesc(level + 1, &desc)) && desc.Width >= cellTexels)
            ++level;
        IDirect3DSurface9* src = 0;
        if (FAILED(source->GetSurfaceLevel(level, &src)))
        {
            cells[c] = 0;
            continue;
        }
        RECT rect = { static_cast<LONG>(x * cellTexels), static_cast<LONG>(z * cellTexels),
                      static_cast<LONG>((x + 1) * cellTexels), static_cast<LONG>((z + 1) * cellTexels) };
        if (SUCCEEDED(D3DXLoadSurfaceFromSurface(dst, 0, &rect, src, 0, 0, D3DX_FILTER_TRIANGLE, 0)))
            ++copied;
        else
            cells[c] = 0;
        src->Release();
    }
    if (dst)
        dst->Release();

    // In linear: the detail factor per cell, and the cells without a colormap (open sea on some
    // levels) filled with the others' mean, not black.
    float mean[3] = { 0.5f, 0.5f, 0.5f };
    D3DLOCKED_RECT locked;
    if (FAILED(texture->LockRect(0, &locked, 0, 0)))
        return false;
    double sum[3] = { 0.0, 0.0, 0.0 };
    double texels = 0.0;
    for (unsigned int y = 0; y != size; ++y)
    {
        unsigned char* row = static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch;
        for (unsigned int x = 0; x != size; ++x)
        {
            const std::size_t c = static_cast<std::size_t>(y / cellTexels) * count + x / cellTexels;
            if (!cells[c])
                continue;
            float rgb[3];
            for (int k = 0; k != 3; ++k)
            {
                rgb[k] = srgbToLinear(row[4 * x + 2 - k] / 255.0f) * factors[3 * c + k];
                row[4 * x + 2 - k] = static_cast<unsigned char>(linearToSrgb(rgb[k]) * 255.0f + 0.5f);
                sum[k] += rgb[k];
            }
            texels += 1.0;
        }
    }
    if (texels > 0.0)
        for (int k = 0; k != 3; ++k)
            mean[k] = static_cast<float>(sum[k] / texels);
    if (copied < count * count)
    {
        unsigned char fill[4] = { 0, 0, 0, 255 };
        for (int k = 0; k != 3; ++k)
            fill[2 - k] = static_cast<unsigned char>(linearToSrgb(mean[k]) * 255.0f + 0.5f);
        for (unsigned int y = 0; y != size; ++y)
        {
            unsigned char* row = static_cast<unsigned char*>(locked.pBits) + y * locked.Pitch;
            for (unsigned int x = 0; x != size; ++x)
                if (!cells[static_cast<std::size_t>(y / cellTexels) * count + x / cellTexels])
                    std::memcpy(row + 4 * x, fill, 4);
        }
    }
    texture->UnlockRect(0);
    D3DXFilterTexture(texture, 0, 0, D3DX_FILTER_BOX);

    // The detail fade's layer: the record weighing most along the map's edge.
    int fade = -1;
    float fadeWeight = 0.0f;
    for (std::size_t i = 0; i != means.edgeWeight.size(); ++i)
        if (means.edgeWeight[i] > fadeWeight && means.valid[i] > 0)
        {
            fadeWeight = means.edgeWeight[i];
            fade = static_cast<int>(i);
        }
    g.constants[6][0] = -1.0f;
    if (fade >= 0 && fade < static_cast<int>(t->m_detailTextures.size()) && t->m_detailTextures[fade])
    {
        const TerrainAtlasEntry& e = atlases->top[fade];
        g.constants[5][0] = e.scaleU;
        g.constants[5][1] = e.scaleV;
        g.constants[5][2] = e.offsetU;
        g.constants[5][3] = e.offsetV;
        // The terrain's detail space is the patch grid (one unit a patch), times the top tiling.
        g.constants[6][0] = static_cast<float>(e.atlas);
        g.constants[6][1] = t->m_detailTextures[fade]->topTiling
                            / (static_cast<float>(t->m_patchSize) * t->m_primaryWorldScale.x);
        g.constants[6][2] = kDetailFadeEnd;
        g.constants[6][3] = 1.0f / kDetailFadeRange;
        for (int k = 0; k != 3; ++k)
            g.constants[7][k] = 1.0f / (std::max)(means.mean[3 * fade + k], 1e-3f);
    }

    // [2]: x the map's half side (m); y colour uv per m; z colour texels per m.
    g.constants[2][0] = mapHalf;
    g.constants[2][1] = 1.0f / (2.0f * mapHalf);
    g.constants[2][2] = static_cast<float>(size) / (2.0f * mapHalf);
    ::renddx9abi::logf("[farground] map colour: %ux%u, %d of %d patches, mean linear %.3f %.3f %.3f; detail fade: record %d",
                       size, size, copied, count * count, mean[0], mean[1], mean[2], fade);
    return true;
}

// The grid's levels, then the strip under the map's edge: a node of the map's edge at each of its
// cells, at the edge height and kSkirtDepth under it, both windings (seen from either side), so no
// gap opens between the map and the far ground where their simplified surfaces differ.
bool buildMesh(float mapHalf, float mapCell)
{
    const int edgeCells = static_cast<int>(std::floor(2.0f * mapHalf / mapCell + 0.5f));
    const unsigned int gridVertices = static_cast<unsigned int>(kLevels * kSide * kSide);
    g.vertexCount = gridVertices + 4u * 2u * static_cast<unsigned int>(edgeCells + 1);
    if (g.vertexCount > 65535u || mapHalf > 32000.0f)
        return false;
    std::vector<unsigned short> indices;
    for (int level = 0; level != kLevels; ++level)
        for (int j = -kCells / 2; j != kCells / 2; ++j)
            for (int i = -kCells / 2; i != kCells / 2; ++i)
            {
                if (level > 0 && i >= -kHole && i < kHole && j >= -kHole && j < kHole)
                    continue;
                const int base = level * kSide * kSide;
                const unsigned short v00 = static_cast<unsigned short>(base + (j + kCells / 2) * kSide + i + kCells / 2);
                const unsigned short v10 = static_cast<unsigned short>(v00 + 1);
                const unsigned short v01 = static_cast<unsigned short>(v00 + kSide);
                const unsigned short v11 = static_cast<unsigned short>(v01 + 1);
                // One diagonal, (0, 0)-(1, 1), everywhere: a morphed 2x2 block becomes the coarse
                // cell's two triangles.
                indices.push_back(v00); indices.push_back(v11); indices.push_back(v10);
                indices.push_back(v00); indices.push_back(v01); indices.push_back(v11);
            }
    for (int side = 0; side != 4; ++side)
        for (int k = 0; k != edgeCells; ++k)
        {
            const unsigned short a = static_cast<unsigned short>(gridVertices + (side * (edgeCells + 1) + k) * 2);
            const unsigned short b = static_cast<unsigned short>(a + 2);
            const unsigned short a1 = static_cast<unsigned short>(a + 1), b1 = static_cast<unsigned short>(b + 1);
            indices.push_back(a); indices.push_back(b); indices.push_back(b1);
            indices.push_back(a); indices.push_back(b1); indices.push_back(a1);
            indices.push_back(a); indices.push_back(b1); indices.push_back(b);
            indices.push_back(a); indices.push_back(a1); indices.push_back(b1);
        }
    g.primCount = static_cast<unsigned int>(indices.size() / 3);

    g.vertexBuffer = dx9rend::bufferManagerCreateVertexBuffer(g.vertexCount, 8u, 8u, "FarGround");
    short* v = g.vertexBuffer ? static_cast<short*>(dx9rend::vertexBufferLock(g.vertexBuffer, 0, 0, 0)) : 0;
    if (!v)
        return false;
    for (int level = 0; level != kLevels; ++level)
        for (int j = 0; j != kSide; ++j)
            for (int i = 0; i != kSide; ++i)
            {
                *v++ = static_cast<short>(i - kCells / 2);
                *v++ = static_cast<short>(j - kCells / 2);
                *v++ = static_cast<short>(level);
                *v++ = 0;
            }
    // The strip: x, z on the map's edge (whole metres), its level slot, the depth under the edge.
    const short half = static_cast<short>(std::floor(mapHalf + 0.5f));
    for (int side = 0; side != 4; ++side)
        for (int k = 0; k <= edgeCells; ++k)
        {
            const short t = static_cast<short>(std::floor(-mapHalf + static_cast<float>(k) * mapCell + 0.5f));
            const short x = side == 0 ? t : (side == 1 ? half : (side == 2 ? t : static_cast<short>(-half)));
            const short z = side == 0 ? static_cast<short>(-half) : (side == 1 ? t : (side == 2 ? half : t));
            for (int row = 0; row != 2; ++row)
            {
                *v++ = x;
                *v++ = z;
                *v++ = static_cast<short>(kSkirtLevel);
                *v++ = static_cast<short>(row ? kSkirtDepth : 0.0f);
            }
        }
    dx9rend::vertexBufferUnlock(g.vertexBuffer);

    g.indexBuffer = dx9rend::bufferManagerCreateIndexBuffer(static_cast<unsigned int>(indices.size()), 8u, 101u);
    void* const ib = g.indexBuffer ? dx9rend::indexBufferLock(g.indexBuffer, 0, 0, 0) : 0;
    if (!ib)
        return false;
    std::memcpy(ib, indices.data(), indices.size() * sizeof(unsigned short));
    dx9rend::indexBufferUnlock(g.indexBuffer);

    static const D3DVERTEXELEMENT9 kDecl[] = { { 0, 0, D3DDECLTYPE_SHORT4, 0, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };
    g.declaration = dx9rend::vertexDeclarationCreate(kDecl);
    return g.declaration != 0;
}

const std::string& techniqueName()
{
    static const std::string name("RaFarGround");
    return name;
}

} // namespace

// The ring's slots load before this (Terrain::loadSurroundingPatches), so their heights are kept.
void farGroundLoad(Terrain* terrain)
{
    releaseBuilt();
    g.pending = terrain;
}

void farGroundAddRing(int slot, const float* x, const float* z, const float* y, unsigned int count, float span,
                      float cell)
{
    if (cell <= 0.0f || span <= 0.0f || slot < 0 || slot >= 8)
        return;
    if (g.ringNodes == 0)
    {
        g.ringSpan = span;
        g.ringCell = cell;
        g.ringNodes = static_cast<int>(std::floor(3.0f * span / cell + 0.5f)) + 1;
        const std::size_t nodes = static_cast<std::size_t>(g.ringNodes) * g.ringNodes;
        g.ringSum.assign(nodes, 0.0f);
        g.ringCount.assign(nodes, 0.0f);
        g.ringSlot.assign(nodes, static_cast<signed char>(-1));
        for (int s = 0; s != 8; ++s)
        {
            g.slotMin[s] = FLT_MAX;
            g.slotMax[s] = -FLT_MAX;
            g.slotHash[s] = 0u;
            g.slotSeen[s] = false;
        }
    }
    // The slot's relief and a hash of its heights in stream order (identical files hash equal).
    unsigned int hash = 2166136261u;
    for (unsigned int k = 0; k != count; ++k)
    {
        g.slotMin[slot] = (std::min)(g.slotMin[slot], y[k]);
        g.slotMax[slot] = (std::max)(g.slotMax[slot], y[k]);
        unsigned int bits;
        std::memcpy(&bits, &y[k], sizeof(bits));
        hash = (hash ^ bits) * 16777619u;
    }
    g.slotHash[slot] = hash ^ count;
    g.slotSeen[slot] = count > 0;
    const float origin = -1.5f * g.ringSpan;
    const int n = g.ringNodes;
    for (unsigned int k = 0; k != count; ++k)
    {
        const int i = static_cast<int>(std::floor((x[k] - origin) / g.ringCell + 0.5f));
        const int j = static_cast<int>(std::floor((z[k] - origin) / g.ringCell + 0.5f));
        if (i < 0 || j < 0 || i >= n || j >= n)
            continue;
        const std::size_t node = static_cast<std::size_t>(j) * n + i;
        g.ringSum[node] += y[k];
        g.ringCount[node] += 1.0f;
        g.ringSlot[node] = static_cast<signed char>(slot);
    }
}

void farGroundUpdate(Terrain* terrain)
{
    if (g.built || !terrain || g.pending != terrain || !device())
        return;
    g.pending = 0;
    dx9rend::Effect* const fx = static_cast<dx9rend::Effect*>(terrain->m_effect);
    world::IHeightmapCluster* const cluster = heightmapCluster();
    if (!fx || !cluster)
        return;
    std::memset(g.constants, 0, sizeof(g.constants));
    g.baked = loadBake();
    const float mapHalf = 0.5f * static_cast<float>(terrain->m_patchCount * terrain->m_patchSize)
                          * terrain->m_primaryWorldScale.x;
    const float mapCell = terrain->m_primaryWorldScale.x > 0.0f ? terrain->m_primaryWorldScale.x : 4.0f;
    const bool ok = buildHeights(terrain, cluster, mapHalf, mapCell) && buildColor(terrain, mapHalf)
                    && buildMesh(mapHalf, mapCell);
    clearRing();
    if (!ok)
    {
        releaseBuilt();
        ::renddx9abi::logf("[farground] not built; the surrounding ring draws");
        return;
    }
    g.constants[0][3] = 0.5f / kEarthRadius;
    g.constants[3][0] = kMorphStart;
    g.constants[3][1] = 1.0f / kMorphRange;
    g.params.levels = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundLevels"));
    g.params.constants = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGround"));
    g.params.height = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundHeight"));
    g.params.heightFar = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundHeightFar"));
    g.params.heightEdge = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundHeightEdge"));
    g.params.color = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundColor"));
    g.built = true;
    ::renddx9abi::logf("[farground] built: map half side %.0f m, %u triangles, %d levels to %.0f m",
                       mapHalf, g.primCount, kLevels, 0.5f * kCells * kCell0 * static_cast<float>(1 << (kLevels - 1)));
}

bool farGroundActive()
{
    const int mode = dx9rend::rendererFarGround();
    return g.built && (mode == 2 || (mode == 1 && g.baked));
}

bool farGroundSeaIn(float x0, float z0, float x1, float z1)
{
    if (!g.built || !g.seaValid || g.farHeight.empty())
        return false;
    const int last = g.coarse.n - 1;
    const auto node = [&](float v) {
        return (std::min)((std::max)(static_cast<int>(std::floor((v - g.coarse.origin) / g.coarse.cell + 0.5f)), 0), last);
    };
    const int i0 = node(x0), i1 = node(x1), j0 = node(z0), j1 = node(z1);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (g.farHeight[static_cast<std::size_t>(j) * g.coarse.n + i] < g.seaLevel)
                return true;
    return false;
}

bool farGroundDraw(Terrain* terrain)
{
    if (!farGroundActive() || !terrain->m_effect
        || !dx9rend::effectHasTechnique(terrain->m_effect, techniqueName()))
        return false;

    // The bake's variant changed (renderer.farGroundScaled): the heights again.
    if (g.baked && g.variant != (dx9rend::rendererFarGroundScaled() ? 1 : 0) && !composeHeights())
        return false;

    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();
    const float* const m = view->getViewMatrixPtr()->m;
    const float camera[3] = { m[12], m[13], m[14] };
    g.constants[0][0] = camera[0];
    g.constants[0][1] = camera[1];
    g.constants[0][2] = camera[2];

    // Each level snapped to twice its cell, so its odd vertices sit on the next level's grid.
    float levels[10][4];
    std::memset(levels, 0, sizeof(levels));
    for (int level = 0; level != kLevels; ++level)
    {
        const float cell = kCell0 * static_cast<float>(1 << level);
        levels[level][0] = std::floor(camera[0] / (2.0f * cell)) * 2.0f * cell;
        levels[level][1] = std::floor(camera[2] / (2.0f * cell)) * 2.0f * cell;
        levels[level][2] = cell;
        levels[level][3] = level + 1 < kLevels ? 1.0f : 0.0f;
    }
    levels[kSkirtLevel][2] = 1.0f;   // the strip: absolute x, z
    param(g.params.levels)->setValue(levels, 0);
    param(g.params.constants)->setValue(g.constants, 0);
    param(g.params.height)->setValue(&g.heightValue, 0);
    param(g.params.heightFar)->setValue(&g.heightFarValue, 0);
    param(g.params.heightEdge)->setValue(&g.heightEdgeValue, 0);
    param(g.params.color)->setValue(&g.colorValue, 0);
    // The detail fade reads the terrain's atlases, as drawFinal binds them.
    const TerrainAtlases* const atlases = terrainAtlases();
    const int atlasCount = atlases ? static_cast<int>(atlases->detail.size()) : 0;
    for (int i = 0; atlasCount && i < 3; ++i)
        param(terrain->m_raTerrain.atlas[i])->setValue(terrainAtlasDetailValue(i < atlasCount ? i : 0), 0);

    dx9rend::effectSetTechniqueByName(terrain->m_effect, techniqueName());
    dx9rend::effectBegin(terrain->m_effect);
    dx9rend::vertexDeclarationBind(g.declaration);
    dx9rend::vertexBufferBindStream(g.vertexBuffer, 0);
    dx9rend::indexBufferBind(g.indexBuffer);
    dx9rend::effectBeginPass(terrain->m_effect, 0u);
    dx9rend::rendererDrawIndexedPrimitive(g.primCount, D3DPT_TRIANGLELIST, 0, 0u, g.vertexCount, 0u);
    dx9rend::effectEnd(terrain->m_effect);
    return true;
}

void farGroundRelease()
{
    releaseBuilt();
    clearRing();
    g.pending = 0;
}

namespace {

void releaseBuilt()
{
    releaseTexture(g.heightTexture, g.heightValue);
    releaseTexture(g.heightFarTexture, g.heightFarValue);
    releaseTexture(g.heightEdgeTexture, g.heightEdgeValue);
    releaseTexture(g.colorTexture, g.colorValue);
    if (g.vertexBuffer)
        dx9rend::vertexBufferRelease(g.vertexBuffer);
    if (g.indexBuffer)
        dx9rend::indexBufferRelease(g.indexBuffer);
    if (g.declaration)
        dx9rend::vertexDeclarationRelease(g.declaration);
    g.vertexBuffer = 0;
    g.indexBuffer = 0;
    g.declaration = 0;
    releaseParam(g.params.levels);
    releaseParam(g.params.constants);
    releaseParam(g.params.height);
    releaseParam(g.params.heightFar);
    releaseParam(g.params.heightEdge);
    releaseParam(g.params.color);
    std::vector<float>().swap(g.farHeight);
    std::vector<float>().swap(g.fineE);
    std::vector<float>().swap(g.fineDist);
    std::vector<float>().swap(g.farE);
    std::vector<float>().swap(g.farDist);
    std::vector<float>().swap(g.edgeMap);
    std::vector<float>().swap(g.bake);
    g.seaValid = false;
    g.baked = false;
    g.built = false;
}

} // namespace

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

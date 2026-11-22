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
#include "FarGround.h"
#include "Terrain.h"
#include "QuadPatch.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
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

// renderer.farGround: the far ground (1) or the retail surrounding ring (0).
bool g_farGround = true;
bool& rendererFarGround() { return g_farGround; }
} // namespace dx9rend

namespace geom {

namespace {

// The mesh: kLevels nested levels of kCells x kCells cells around the camera, cell kCell0 * 2^level.
// Level 0 is whole; each coarser one leaves out its middle (kHole cells from its centre on each
// side), which the finer level covers with one coarse cell to spare whatever their snapping.
const int   kLevels = 7;
const int   kCells = 64;
const int   kHole = kCells / 4 - 1;
const float kCell0 = 16.0f;
const int   kSide = kCells + 1;                                    // vertices a side
const float kMorphStart = 16.0f;                                   // cells from the camera
const float kMorphRange = 10.0f;                                   // complete 26 cells out
const float kFade = 3000.0f;                                       // m past the ring to the base height
const float kEarthRadius = 6371000.0f;
const unsigned int kColorTexels = 512;                             // the map's colour, texels a side (at most)

struct Params
{
    void* levels;    // RaFarGroundLevels (float4[8]: origin x, z, cell, morphs)
    void* constants; // RaFarGround (float4[4], layout below)
    void* height;    // RaFarGroundHeight (texture)
    void* color;     // RaFarGroundColor (texture)
};

struct State
{
    // The ring's heights as the slots load: a grid over the ring's square, node (i, j) at
    // (origin + i * cell, origin + j * cell).
    float              ringSpan;
    float              ringCell;
    int                ringNodes;
    std::vector<float> ringSum;
    std::vector<float> ringCount;

    Terrain*           pending;
    bool               built;
    IDirect3DTexture9* heightTexture;
    IDirect3DTexture9* colorTexture;
    dx9rend::EngineTexture heightValue;
    dx9rend::EngineTexture colorValue;
    void*              vertexBuffer;
    void*              indexBuffer;
    void*              declaration;
    unsigned int       vertexCount;
    unsigned int       primCount;
    Params             params;
    float              constants[4][4];
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

void releaseParam(void*& handle)
{
    if (handle)
        param(handle)->release();
    handle = 0;
}

void clearRing()
{
    g.ringSpan = 0.0f;
    g.ringCell = 0.0f;
    g.ringNodes = 0;
    std::vector<float>().swap(g.ringSum);
    std::vector<float>().swap(g.ringCount);
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

// The base height: the median of the ring's outer edge over the slots the level has, else the
// map's lowest height.
float baseHeight(const Terrain* t)
{
    std::vector<float> edges;
    for (int s = 0; s != 8; ++s)
        if (t->m_surrounding[s] && t->m_surroundOuterEdge[s] < 0.5f * FLT_MAX)
            edges.push_back(t->m_surroundOuterEdge[s]);
    if (edges.empty())
        return t->m_minHeight;
    std::sort(edges.begin(), edges.end());
    return edges[edges.size() / 2];
}

bool buildHeight(Terrain* t, float base, float mapHalf)
{
    int n = g.ringNodes;
    std::vector<float> value, weight;
    if (n > 1)
    {
        value.resize(g.ringSum.size());
        weight.resize(g.ringSum.size());
        int known = 0;
        for (std::size_t k = 0; k != value.size(); ++k)
        {
            weight[k] = g.ringCount[k] > 0.0f ? 1.0f : 0.0f;
            value[k] = g.ringCount[k] > 0.0f ? g.ringSum[k] / g.ringCount[k] : 0.0f;
            known += g.ringCount[k] > 0.0f;
        }
        pullPush(value, weight, n);
        ::renddx9abi::logf("[farground] ring heights: %dx%d nodes of %.1f m, %d from the ring's vertices",
                           n, n, g.ringCell, known);
    }
    else
    {
        n = 1;
        value.assign(1, base);
        ::renddx9abi::logf("[farground] no surrounding ring: flat at %.1f m", base);
    }
    IDirect3DTexture9* texture = 0;
    if (FAILED(device()->CreateTexture(n, n, 1, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &texture, 0)))
    {
        ::renddx9abi::logf("[farground] could not create the %dx%d height texture", n, n);
        return false;
    }
    D3DLOCKED_RECT rect;
    if (FAILED(texture->LockRect(0, &rect, 0, 0)))
    {
        texture->Release();
        return false;
    }
    for (int j = 0; j != n; ++j)
        std::memcpy(static_cast<unsigned char*>(rect.pBits) + j * rect.Pitch, &value[static_cast<std::size_t>(j) * n],
                    sizeof(float) * n);
    texture->UnlockRect(0);
    g.heightTexture = texture;
    g.heightValue.baseTexture = texture;

    // [1]: height uv = xz * x + y; z the ring's half side (m); w 1 / the fade past it.
    const float cell = n > 1 ? g.ringCell : 1.0f;
    const float half = n > 1 ? 1.5f * g.ringSpan : mapHalf;
    const float origin = n > 1 ? -half : 0.0f;
    g.constants[1][0] = 1.0f / (n * cell);
    g.constants[1][1] = (-origin / cell + 0.5f) / n;
    g.constants[1][2] = half;
    g.constants[1][3] = 1.0f / kFade;
    g.constants[3][2] = n > 1 ? cell : 16.0f;
    return true;
}

// The playable map's colour: each patch's colormap, at the smallest level that still covers its
// cell, scaled into the patch's cell (column = patch x, row = patch z, as the lightmap atlas).
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
    IDirect3DSurface9* dst = 0;
    texture->GetSurfaceLevel(0, &dst);
    int copied = 0;
    for (int p = 0; dst && p != count * count; ++p)
    {
        QuadPatch* const patch = t->m_patches[p];
        IDirect3DTexture9* const source = patch ? d3dTexture(patch->colormap) : 0;
        if (!source)
            continue;
        int x = p % count, z = p / count;
        if (patch->gridX + count / 2 >= 0 && patch->gridX + count / 2 < count
            && patch->gridZ + count / 2 >= 0 && patch->gridZ + count / 2 < count)
        {
            x = patch->gridX + count / 2;
            z = patch->gridZ + count / 2;
        }
        unsigned int level = 0;
        D3DSURFACE_DESC desc = {};
        while (level + 1 < source->GetLevelCount()
               && SUCCEEDED(source->GetLevelDesc(level + 1, &desc)) && desc.Width >= cellTexels)
            ++level;
        IDirect3DSurface9* src = 0;
        if (FAILED(source->GetSurfaceLevel(level, &src)))
            continue;
        RECT rect = { static_cast<LONG>(x * cellTexels), static_cast<LONG>(z * cellTexels),
                      static_cast<LONG>((x + 1) * cellTexels), static_cast<LONG>((z + 1) * cellTexels) };
        if (SUCCEEDED(D3DXLoadSurfaceFromSurface(dst, 0, &rect, src, 0, 0, D3DX_FILTER_TRIANGLE, 0)))
            ++copied;
        src->Release();
    }
    if (dst)
        dst->Release();
    D3DXFilterTexture(texture, 0, 0, D3DX_FILTER_BOX);
    g.colorTexture = texture;
    g.colorValue.baseTexture = texture;
    ::renddx9abi::logf("[farground] map colour: %ux%u, %d of %d patches", size, size, copied, count * count);

    // [2]: x the map's half side (m); y colour uv per m; z colour texels per m; w the base height.
    g.constants[2][0] = mapHalf;
    g.constants[2][1] = 1.0f / (2.0f * mapHalf);
    g.constants[2][2] = static_cast<float>(size) / (2.0f * mapHalf);
    return true;
}

bool buildMesh()
{
    g.vertexCount = static_cast<unsigned int>(kLevels * kSide * kSide);
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

void farGroundAddRing(const float* x, const float* z, const float* y, unsigned int count, float span, float cell)
{
    if (cell <= 0.0f || span <= 0.0f)
        return;
    if (g.ringNodes == 0)
    {
        g.ringSpan = span;
        g.ringCell = cell;
        g.ringNodes = static_cast<int>(std::floor(3.0f * span / cell + 0.5f)) + 1;
        const std::size_t nodes = static_cast<std::size_t>(g.ringNodes) * g.ringNodes;
        g.ringSum.assign(nodes, 0.0f);
        g.ringCount.assign(nodes, 0.0f);
    }
    const float origin = -1.5f * g.ringSpan;
    const int n = g.ringNodes;
    for (unsigned int k = 0; k != count; ++k)
    {
        const int i = static_cast<int>(std::floor((x[k] - origin) / g.ringCell + 0.5f));
        const int j = static_cast<int>(std::floor((z[k] - origin) / g.ringCell + 0.5f));
        if (i < 0 || j < 0 || i >= n || j >= n)
            continue;
        g.ringSum[static_cast<std::size_t>(j) * n + i] += y[k];
        g.ringCount[static_cast<std::size_t>(j) * n + i] += 1.0f;
    }
}

void farGroundUpdate(Terrain* terrain)
{
    if (g.built || !terrain || g.pending != terrain || !device())
        return;
    g.pending = 0;
    dx9rend::Effect* const fx = static_cast<dx9rend::Effect*>(terrain->m_effect);
    if (!fx)
        return;
    std::memset(g.constants, 0, sizeof(g.constants));
    const float mapHalf = 0.5f * static_cast<float>(terrain->m_patchCount * terrain->m_patchSize)
                          * terrain->m_primaryWorldScale.x;
    const float base = baseHeight(terrain);
    const bool ok = buildHeight(terrain, base, mapHalf) && buildColor(terrain, mapHalf) && buildMesh();
    clearRing();
    if (!ok)
    {
        ::renddx9abi::logf("[farground] not built; the surrounding ring draws");
        return;
    }
    g.constants[2][3] = base;
    g.constants[0][3] = 0.5f / kEarthRadius;
    g.constants[3][0] = kMorphStart;
    g.constants[3][1] = 1.0f / kMorphRange;
    g.params.levels = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundLevels"));
    g.params.constants = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGround"));
    g.params.height = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundHeight"));
    g.params.color = dx9rend::effectGetShaderParameter(fx, std::string("RaFarGroundColor"));
    g.built = true;
    ::renddx9abi::logf("[farground] built: map half side %.0f m, base height %.1f m, %u triangles, "
                       "%d levels to %.0f m",
                       mapHalf, base, g.primCount, kLevels, 0.5f * kCells * kCell0 * static_cast<float>(1 << (kLevels - 1)));
}

bool farGroundDraw(Terrain* terrain)
{
    if (!dx9rend::rendererFarGround() || !g.built || !terrain->m_effect
        || !dx9rend::effectHasTechnique(terrain->m_effect, techniqueName()))
        return false;

    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();
    const float* const m = view->getViewMatrixPtr()->m;
    const float camera[3] = { m[12], m[13], m[14] };
    g.constants[0][0] = camera[0];
    g.constants[0][1] = camera[1];
    g.constants[0][2] = camera[2];

    // Each level snapped to twice its cell, so its odd vertices sit on the next level's grid.
    float levels[8][4];
    std::memset(levels, 0, sizeof(levels));
    for (int level = 0; level != kLevels; ++level)
    {
        const float cell = kCell0 * static_cast<float>(1 << level);
        levels[level][0] = std::floor(camera[0] / (2.0f * cell)) * 2.0f * cell;
        levels[level][1] = std::floor(camera[2] / (2.0f * cell)) * 2.0f * cell;
        levels[level][2] = cell;
        levels[level][3] = level + 1 < kLevels ? 1.0f : 0.0f;
    }
    param(g.params.levels)->setValue(levels, 0);
    param(g.params.constants)->setValue(g.constants, 0);
    param(g.params.height)->setValue(&g.heightValue, 0);
    param(g.params.color)->setValue(&g.colorValue, 0);

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
    if (g.heightTexture)
        g.heightTexture->Release();
    if (g.colorTexture)
        g.colorTexture->Release();
    g.heightTexture = 0;
    g.colorTexture = 0;
    g.heightValue.baseTexture = 0;
    g.colorValue.baseTexture = 0;
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
    releaseParam(g.params.color);
    g.built = false;
}

} // namespace

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

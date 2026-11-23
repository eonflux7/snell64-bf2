// [RENDER] Water's open-water map (render milestone 23, task 2, docs/milestones/render-23-water-shore.md;
// contract 38). Not part of retail BF2, whose water was the same everywhere.
//
// Baked once a level has loaded (rendererWaterOpenLoad marks it, the next water frame samples the
// heightmap cluster on the main thread and a worker does the rest) from the terrain's heights and
// the sea level, over the map's square in kSide x kSide texels:
//
//   r   the signed distance to the shore, metres: positive over water (height under the sea level),
//       negative on land; from two exact Euclidean distance transforms (Felzenszwalb and
//       Huttenlocher, "Distance transforms of sampled functions", 2012), each texel's centre to the
//       nearest texel centre of the other kind, less half a texel
//   g b the direction toward open water: the distance's gradient (central differences), unit, zero
//       where it vanishes
//   a   log2 of the effective fetch, metres: how far the wind has blown over water to reach the
//       texel. The wind's direction is the level's WaterScroll (+x without one). The fetch along
//       each of kRadials radials within 42 degrees of the wind, combined as the Shore Protection
//       Manual's effective fetch (US Army Corps of Engineers, 1984, after Saville 1962): weighted
//       by cos^2 of the radial's angle to the wind, here normalised so uniform fetch is itself.
//       Each radial's fetch is one sweep over the map: a texel's is the upwind texel's (linear
//       between the two texels a step upwind) plus the step, zero on land. Past the map, water
//       along an edge is open sea (kOpenFetch) where its run along that edge is at least kOpenRun,
//       a shore otherwise (a river leaving the map)
//
// Two rows below the map hold a table over log2 fetch, kTable entries from kTableFirst (one per
// octave), written by Water.cpp whenever the spectrum changes (rendererWaterOpenTable): row 0 each
// wave cascade's slope amplitude against the level's fetch and the significant height's ratio;
// row 1 the whitecap coverage's ratio. The texture is A16B16G16R16F, kSide x (kSide + 2), managed,
// one level; while the water queue draws with waves it is on kWaterOpenSampler (bilinear, clamp)
// and RaWaterFoam.w is 1 / the map's side in metres (0 without it). The map is centred on the
// world's origin, row j at z, column i at x.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/PluginAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../Geom/Terrain.h"
#include "../Physics/HeightmapCluster.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

const int kSide = 1024;              // the map's texels per side
const int kTexRows = kSide + 2;      // and the table's two rows
const int kRadials = 15;             // the effective fetch's radials, kRadialStep apart about the wind
const double kRadialStep = 6.0;      // degrees
const double kOpenRun = 1000.0;      // m of water along a map edge that continues as open sea past it
const float kOpenFetch = 1.0e6f;     // m: open sea's fetch, past renderer.waterFetch's range
const double kPi = 3.14159265358979323846;

struct Bake
{
    std::vector<float> heights;      // kSide^2, row j at z
    float sea, half;
    double dirX, dirZ;
    std::vector<float> texels;       // kSide^2 x 4, the map's r g b a
    double waterShare, meanLog2Fetch, ms;
    int openEdges;                   // edge runs taken as open sea
};

struct OpenState
{
    bool pending;                    // a level loaded: bake on the next water frame
    bool haveKey;
    float keySea, keyHalf;
    double keyDirX, keyDirZ;
    std::thread thread;
    std::atomic<bool> done;
    bool building;
    Bake bake;                       // the worker's while building
    std::vector<float> map;          // the last bake's texels
    Bake info;                       // its inputs and statistics (no arrays)
    bool mapNew;                     // not uploaded yet
    float table[kWaterOpenTableRows][kWaterOpenTable][4];
    bool haveTable, tableNew;
    IDirect3DTexture9* texture;
    float scale;                     // 1 / the uploaded map's side, m
};

OpenState* g_open = 0;
OpenState& st()
{
    if (!g_open)
    {
        g_open = new OpenState();
        g_open->done = false;
    }
    return *g_open;
}

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

world::IHeightmapCluster* heightmapCluster()
{
    IBase* const found = ::dice::hfe::classManager->getSingleton(::renddx9abi::HostString("HeightmapCluster"));
    return static_cast<world::IHeightmapCluster*>(static_cast<void*>(found));
}

double msSince(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? 1000.0 * static_cast<double>(t.QuadPart - t0.QuadPart) / f.QuadPart : 0.0;
}

// ---- the bake (worker) ----

// The squared distance transform of a sampled function along one line (Felzenszwalb and
// Huttenlocher): d[q] = min over p of (q - p)^2 + f[p].
void distance1d(const std::vector<float>& f, std::vector<float>& d, int n, std::vector<int>& v,
                std::vector<float>& z)
{
    const float inf = 1.0e20f;
    int k = 0;
    v[0] = 0;
    z[0] = -inf;
    z[1] = inf;
    for (int q = 1; q < n; ++q)
    {
        float s = ((f[q] + static_cast<float>(q) * q) - (f[v[k]] + static_cast<float>(v[k]) * v[k]))
                / (2.0f * (q - v[k]));
        while (s <= z[k] && k > 0)
        {
            --k;
            s = ((f[q] + static_cast<float>(q) * q) - (f[v[k]] + static_cast<float>(v[k]) * v[k]))
              / (2.0f * (q - v[k]));
        }
        if (s <= z[k])
        {
            v[k] = q;   // lower everywhere than the only parabola left
            continue;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = inf;
    }
    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < q)
            ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = dq * dq + f[v[k]];
    }
}

// The distance, texels, from each texel's centre to the nearest texel where `to` is true.
void distanceTo(const std::vector<unsigned char>& to, bool want, std::vector<float>& out)
{
    const float inf = 1.0e20f;
    out.assign(static_cast<std::size_t>(kSide) * kSide, inf);
    std::vector<float> f(kSide), d(kSide), z(kSide + 1);
    std::vector<int> v(kSide);
    for (int j = 0; j < kSide; ++j)
    {
        for (int i = 0; i < kSide; ++i)
            f[i] = (to[static_cast<std::size_t>(j) * kSide + i] != 0) == want ? 0.0f : inf;
        distance1d(f, d, kSide, v, z);
        for (int i = 0; i < kSide; ++i)
            out[static_cast<std::size_t>(j) * kSide + i] = d[i];
    }
    for (int i = 0; i < kSide; ++i)
    {
        for (int j = 0; j < kSide; ++j)
            f[j] = out[static_cast<std::size_t>(j) * kSide + i];
        distance1d(f, d, kSide, v, z);
        for (int j = 0; j < kSide; ++j)
            out[static_cast<std::size_t>(j) * kSide + i] = d[j] >= inf ? inf : std::sqrt(d[j]);
    }
}

// Which texels along each edge continue as open sea: water in a run along that edge of at least
// kOpenRun. Edges 0 x low (i = 0), 1 x high, 2 z low (j = 0), 3 z high; indexed along the edge.
int openEdges(const std::vector<unsigned char>& water, double cell, std::vector<unsigned char> open[4])
{
    int runs = 0;
    for (int e = 0; e < 4; ++e)
    {
        open[e].assign(kSide, 0);
        int start = -1;
        for (int t = 0; t <= kSide; ++t)
        {
            bool w = false;
            if (t < kSide)
            {
                const int i = e == 0 ? 0 : e == 1 ? kSide - 1 : t;
                const int j = e == 2 ? 0 : e == 3 ? kSide - 1 : t;
                w = water[static_cast<std::size_t>(j) * kSide + i] != 0;
            }
            if (w && start < 0)
                start = t;
            if (!w && start >= 0)
            {
                if ((t - start) * cell >= kOpenRun)
                {
                    std::fill(open[e].begin() + start, open[e].begin() + t, static_cast<unsigned char>(1));
                    ++runs;
                }
                start = -1;
            }
        }
    }
    return runs;
}

// One radial's fetch, metres, for the wind blowing toward (dx, dz): a sweep from the upwind side,
// a texel's fetch the upwind one's (linear between the two texels one column or row upwind) plus
// the step, zero on land; past the map the edge's open sea or a shore.
void fetchSweep(const std::vector<unsigned char>& water, const std::vector<unsigned char> open[4], double cell,
                double dx, double dz, std::vector<float>& out)
{
    out.assign(static_cast<std::size_t>(kSide) * kSide, 0.0f);
    const bool alongX = std::fabs(dx) >= std::fabs(dz);
    // Sweep along the major axis a; the minor axis b moves `slope` texels per texel of a
    const double major = alongX ? dx : dz, minor = alongX ? dz : dx;
    const int stepA = major >= 0.0 ? 1 : -1;
    const double slope = minor / std::fabs(major);
    const float stepLength = static_cast<float>(cell * std::sqrt(1.0 + slope * slope));
    const int firstA = stepA > 0 ? 0 : kSide - 1;
    auto at = [&](int a, int b) -> std::size_t {
        return alongX ? static_cast<std::size_t>(b) * kSide + a : static_cast<std::size_t>(a) * kSide + b;
    };
    // The edge the upwind ray leaves by: the major axis's upwind edge, or the minor's
    const int edgeMajor = alongX ? (stepA > 0 ? 0 : 1) : (stepA > 0 ? 2 : 3);
    const int edgeMinorLow = alongX ? 2 : 0, edgeMinorHigh = alongX ? 3 : 1;
    for (int n = 0; n < kSide; ++n)
    {
        const int a = firstA + n * stepA;
        for (int b = 0; b < kSide; ++b)
        {
            const std::size_t k = at(a, b);
            if (!water[k])
                continue;
            float upwind;
            const double ub = b - slope;   // the upwind point's minor coordinate
            if (n == 0)
                upwind = open[edgeMajor][b] ? kOpenFetch : 0.0f;
            else if (ub < 0.0)
                upwind = open[edgeMinorLow][a - stepA] ? kOpenFetch : 0.0f;
            else if (ub > kSide - 1)
                upwind = open[edgeMinorHigh][a - stepA] ? kOpenFetch : 0.0f;
            else
            {
                const int b0 = std::min(static_cast<int>(ub), kSide - 2);
                const float t = static_cast<float>(ub - b0);
                upwind = out[at(a - stepA, b0)] * (1.0f - t) + out[at(a - stepA, b0 + 1)] * t;
            }
            out[k] = std::min(upwind + stepLength, kOpenFetch);
        }
    }
}

void runBake(Bake& b)
{
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const std::size_t count = static_cast<std::size_t>(kSide) * kSide;
    const double cell = 2.0 * b.half / kSide;
    std::vector<unsigned char> water(count);
    std::size_t wet = 0;
    for (std::size_t k = 0; k < count; ++k)
    {
        water[k] = b.heights[k] < b.sea ? 1 : 0;
        wet += water[k];
    }
    b.waterShare = static_cast<double>(wet) / count;

    // The signed distance, metres
    std::vector<float> toLand, toWater, sd(count);
    distanceTo(water, false, toLand);
    distanceTo(water, true, toWater);
    const float big = static_cast<float>(4.0 * b.half);
    for (std::size_t k = 0; k < count; ++k)
    {
        const float d = water[k] ? toLand[k] : toWater[k];
        const float m = d >= 1.0e19f ? big : static_cast<float>((d - 0.5) * cell);
        sd[k] = water[k] ? m : -m;
    }
    std::vector<float>().swap(toLand);
    std::vector<float>().swap(toWater);

    // The effective fetch
    std::vector<unsigned char> open[4];
    b.openEdges = openEdges(water, cell, open);
    std::vector<double> sum(count, 0.0);
    std::vector<float> radial;
    double weights = 0.0;
    const double wind = std::atan2(b.dirZ, b.dirX);
    for (int r = 0; r < kRadials; ++r)
    {
        const double offset = (r - (kRadials - 1) / 2) * kRadialStep * kPi / 180.0;
        const double c = std::cos(offset);
        fetchSweep(water, open, cell, std::cos(wind + offset), std::sin(wind + offset), radial);
        for (std::size_t k = 0; k < count; ++k)
            sum[k] += c * c * radial[k];
        weights += c * c;
    }

    b.texels.assign(count * 4, 0.0f);
    double logSum = 0.0;
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j) * kSide + i;
            const float gx = sd[static_cast<std::size_t>(j) * kSide + std::min(i + 1, kSide - 1)]
                           - sd[static_cast<std::size_t>(j) * kSide + std::max(i - 1, 0)];
            const float gz = sd[static_cast<std::size_t>(std::min(j + 1, kSide - 1)) * kSide + i]
                           - sd[static_cast<std::size_t>(std::max(j - 1, 0)) * kSide + i];
            const float len = std::sqrt(gx * gx + gz * gz);
            const float fetch = static_cast<float>(sum[k] / weights);
            float* const p = &b.texels[k * 4];
            p[0] = sd[k];
            p[1] = len > 1e-6f ? gx / len : 0.0f;
            p[2] = len > 1e-6f ? gz / len : 0.0f;
            p[3] = std::log2(std::max(fetch, 1.0f));
            if (water[k])
                logSum += p[3];
        }
    b.meanLog2Fetch = wet ? logSum / wet : 0.0;
    b.ms = msSince(t0);
}

// ---- the texture ----

bool ensureTexture(OpenState& s)
{
    if (s.texture)
        return true;
    IDirect3DDevice9* const d = device();
    if (!d || d->CreateTexture(kSide, kTexRows, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &s.texture, 0) < 0)
    {
        s.texture = 0;
        return false;
    }
    return true;
}

bool uploadRows(OpenState& s, int row0, int rows, int width, const float* texels)
{
    RECT r = { 0, row0, width, row0 + rows };
    D3DLOCKED_RECT locked;
    if (s.texture->LockRect(0, &locked, &r, 0) < 0)
        return false;
    for (int j = 0; j < rows; ++j)
        D3DXFloat32To16Array(reinterpret_cast<D3DXFLOAT16*>(static_cast<unsigned char*>(locked.pBits)
                                                            + static_cast<std::size_t>(j) * locked.Pitch),
                             texels + static_cast<std::size_t>(j) * width * 4, width * 4);
    s.texture->UnlockRect(0);
    return true;
}

void joinBake(OpenState& s)
{
    if (s.thread.joinable())
        s.thread.join();
    s.building = false;
}

} // namespace

void rendererWaterOpenLoad()
{
    OpenState& s = st();
    s.pending = true;
    s.haveKey = false;
}

void rendererWaterOpenRelease()
{
    if (!g_open)
        return;
    OpenState& s = *g_open;
    joinBake(s);
    std::vector<float>().swap(s.bake.heights);
    std::vector<float>().swap(s.bake.texels);
    std::vector<float>().swap(s.map);
    if (s.texture)
        s.texture->Release();
    s.texture = 0;
    s.scale = 0.0f;
    s.pending = false;
    s.haveKey = false;
    s.mapNew = false;
    s.tableNew = s.haveTable;
}

void rendererWaterOpenTable(const float table[kWaterOpenTableRows][kWaterOpenTable][4])
{
    OpenState& s = st();
    std::memcpy(s.table, table, sizeof(s.table));
    s.haveTable = true;
    s.tableNew = true;
}

void rendererWaterOpenFrame(double windX, double windZ)
{
    OpenState& s = st();
    if (s.building && s.done)
    {
        joinBake(s);
        s.map.swap(s.bake.texels);
        std::vector<float>().swap(s.bake.heights);
        std::vector<float>().swap(s.bake.texels);
        s.info = s.bake;
        s.mapNew = true;
        std::fprintf(stderr, "[WATER] open-water map: %d x %d over %.0f m, sea level %.2f m, wind toward "
                     "(%.2f, %.2f): water %.1f %% of the map, %d edge runs open sea, mean effective fetch "
                     "%.0f m; baked in %.0f ms\n", kSide, kSide, 2.0 * s.info.half, s.info.sea,
                     s.info.dirX, s.info.dirZ, 100.0 * s.info.waterShare, s.info.openEdges,
                     std::exp2(s.info.meanLog2Fetch), s.info.ms);
    }

    // A new level, or the wind turned: sample the heights here, bake on a worker
    geom::Terrain* const t = geom::terrain();
    world::IHeightmapCluster* const cluster = heightmapCluster();
    if (t && cluster && cluster->getPrimaryHeightmap() && !s.building && (s.pending || s.haveKey))
    {
        const float half = 0.5f * static_cast<float>(t->m_patchCount * t->m_patchSize) * t->m_primaryWorldScale.x;
        const float sea = cluster->getSeaWaterLevel();
        const bool changed = !s.haveKey || sea != s.keySea || half != s.keyHalf || windX != s.keyDirX
                          || windZ != s.keyDirZ;
        if (changed && half > 0.0f)
        {
            s.pending = false;
            s.haveKey = true;
            s.keySea = sea;
            s.keyHalf = half;
            s.keyDirX = windX;
            s.keyDirZ = windZ;
            Bake& b = s.bake;
            b.sea = sea;
            b.half = half;
            b.dirX = windX;
            b.dirZ = windZ;
            b.heights.resize(static_cast<std::size_t>(kSide) * kSide);
            const float cell = 2.0f * half / kSide, inside = half - 0.01f;
            for (int j = 0; j < kSide; ++j)
                for (int i = 0; i < kSide; ++i)
                {
                    const float x = std::min(-half + (i + 0.5f) * cell, inside);
                    const float z = std::min(-half + (j + 0.5f) * cell, inside);
                    b.heights[static_cast<std::size_t>(j) * kSide + i] = cluster->getHeightInWorldCoords(x, z);
                }
            s.done = false;
            s.building = true;
            s.thread = std::thread([]() {
                OpenState& w = st();
                runBake(w.bake);
                w.done = true;
            });
        }
    }

    if ((s.mapNew || s.tableNew) && !s.map.empty() && ensureTexture(s))
    {
        bool ok = true;
        if (s.mapNew)
        {
            ok = uploadRows(s, 0, kSide, kSide, s.map.data());
            s.mapNew = !ok;
            s.scale = ok ? 0.5f / s.info.half : 0.0f;
        }
        if (s.tableNew && s.haveTable && ok)
            s.tableNew = !uploadRows(s, kSide, kWaterOpenTableRows, kWaterOpenTable, &s.table[0][0][0]);
    }
}

bool g_waterOpen = true;
bool& rendererWaterOpen() { return g_waterOpen; }

float rendererWaterOpenBind()
{
    OpenState& s = st();
    IDirect3DDevice9* const d = device();
    if (!d || !g_waterOpen || !s.texture || s.scale <= 0.0f || !s.haveTable || s.tableNew)
        return 0.0f;
    d->SetTexture(kWaterOpenSampler, s.texture);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kWaterOpenSampler, D3DSAMP_SRGBTEXTURE, FALSE);
    return s.scale;
}

void rendererWaterOpenUnbind()
{
    if (IDirect3DDevice9* const d = device())
        d->SetTexture(kWaterOpenSampler, 0);
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

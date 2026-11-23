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
//   a   log2 of the effective fetch upwind, metres: the fetch whose sea has the energy the texel's
//       waves bring in from every direction. Along each of kWaterOpenRadials radials (6 degrees
//       apart, within 90 degrees of the wind) the fetch is how far the wind has blown over water to
//       reach the texel, at most the level's (renderer.waterFetch: past it the sea is the level's).
//       A fetch-limited JONSWAP sea's variance goes as F^(4/3 - 0.22) (alpha ~ F^-0.22, omega_p
//       ~ F^-1/3, the shape fixed), so each radial's energy is (F / F_level)^kEnergyPower; they
//       are averaged with the spectrum's spreading weighted by energy (Water.cpp, Mitsuyasu's),
//       the share of the sea travelling along each, and the mean energy turned back into a fetch.
//       A lee fills in gradually from the directions that clear the shore, as diffraction round a
//       breakwater's tip would spread it (not modelled itself). An average of the fetches instead
//       (the Shore Protection Manual's effective fetch) lets one open radial carry a whole sea.
//       Each radial's fetch is one sweep over the map: a texel's is the upwind texel's (linear
//       between the two texels a step upwind) plus the step, zero on land. Past the map, water
//       along an edge is open sea (kOpenFetch) where its run along that edge is at least kOpenRun,
//       a shore otherwise (a river leaving the map)
//
// The wind's direction is Water.cpp's (renderer.waterWindFrom), or chosen here when automatic: the
// one among kWindCandidates bringing the most wave energy to the map's water, the wind from its
// most open sea (a sea breeze, the daytime wind of a warm coast; along a river's longest reach
// inland). The mean energy over the water is linear in the radials', so each direction is swept
// once and every candidate's score is its radials' mean energies weighted by the spreading.
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
#include "ToolsAtmosphere.h"
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
const int kRadials = kWaterOpenRadials;   // about the wind, kWaterOpenRadialStep apart
const int kWindCandidates = 60;      // automatic wind directions, kWaterOpenRadialStep apart
const double kEnergyPower = 4.0 / 3.0 - 0.22;   // a fetch-limited JONSWAP sea's variance ~ F^this
const double kOpenRun = 1000.0;      // m of water along a map edge that continues as open sea past it
const float kOpenFetch = 1.0e6f;     // m: open sea's fetch, past renderer.waterFetch's range
const double kPi = 3.14159265358979323846;

struct Bake
{
    std::vector<float> heights;      // kSide^2, row j at z
    float sea, half;
    bool automatic;                  // choose the wind's direction (dirX, dirZ out) or take it (in)
    double dirX, dirZ;
    double fetch;                    // the level's, m
    double wavelength;               // its sea's peak, m
    float spreading[kWaterOpenRadials];
    std::vector<float> texels;       // kSide^2 x 4, the map's r g b a
    double waterShare, meanLog2Fetch, ms;
    int openEdges;                   // edge runs taken as open sea
};

struct OpenState
{
    bool pending;                    // a level loaded: bake on the next water frame
    bool haveKey;
    float keySea, keyHalf;
    bool keyAutomatic;
    double keyDirX, keyDirZ, keyFetch, keyWavelength;
    float keySpreading[kWaterOpenRadials];
    bool haveDir;                    // a bake's direction (automatic or given) for rendererWaterOpenWind
    std::thread thread;
    std::atomic<bool> done;
    bool building;
    Bake bake;                       // the worker's while building
    std::vector<float> map;          // the last bake's texels (kept for tools.water)
    Bake info;                       // its inputs and statistics (no arrays)
    bool mapNew;                     // not uploaded yet
    float table[kWaterOpenTableRows][kWaterOpenTable][4];
    float spreading[kWaterOpenRadials];
    double fetch, wavelength;
    bool haveTable, tableNew;
    IDirect3DTexture9* texture;
    float scale;                     // 1 / the uploaded map's side, m
    std::string message;
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
//
// With spread > 0 (m^2 per m travelled) the waves diffract: what arrives from upwind spreads
// across the ray as it travels, so a shadow's edge behind a breakwater's tip blurs over
// sqrt(spread * distance) rather than staying a knife edge. The parabolic approximation of
// diffraction is a diffusion across the ray; on energy (here (F / cap)^kEnergyPower, the fetch
// being the sea it carries) the Fresnel edge's 20-80 % width, about 0.85 sqrt(lambda x), is an
// erf's of variance lambda x / 4, so the caller passes lambda / 4. Each row across the sweep is
// solved implicitly (unconditionally stable at any texel size), land ends a segment (reflecting).
void fetchSweep(const std::vector<unsigned char>& water, const std::vector<unsigned char> open[4], double cell,
                double dx, double dz, std::vector<float>& out, double spread = 0.0, double cap = 0.0)
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
    // Diffusion per step in texels across: adjacent texels of a row are cell / sqrt(1 + slope^2)
    // apart across the ray, and the ray travels stepLength
    const bool diffract = spread > 0.0 && cap > 0.0;
    const double across = cell / std::sqrt(1.0 + slope * slope);
    const double k = diffract ? 0.5 * spread * stepLength / (across * across) : 0.0;
    std::vector<double> row(kSide), cPrime(kSide);
    for (int n = 0; n < kSide; ++n)
    {
        const int a = firstA + n * stepA;
        for (int b = 0; b < kSide; ++b)
        {
            const std::size_t i = at(a, b);
            if (!water[i])
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
            if (diffract)
                row[b] = std::pow(std::min(static_cast<double>(upwind), cap) / cap, kEnergyPower);
            else
                out[i] = std::min(upwind + stepLength, kOpenFetch);
        }
        if (!diffract)
            continue;
        // Each run of water across the row: (1 + 2k) E_b - k E_b-1 - k E_b+1 = E_upwind, Thomas's
        // algorithm; at a run's ends the missing neighbour reflects
        for (int b0 = 0; b0 < kSide;)
        {
            if (!water[at(a, b0)])
            {
                ++b0;
                continue;
            }
            int b1 = b0;
            while (b1 + 1 < kSide && water[at(a, b1 + 1)])
                ++b1;
            double prev = 0.0;
            for (int b = b0; b <= b1; ++b)
            {
                const double lower = b > b0 ? -k : 0.0, upper = b < b1 ? -k : 0.0;
                const double diag = 1.0 - lower - upper;
                const double m = diag - lower * prev;
                cPrime[b] = upper / m;
                row[b] = (row[b] - lower * (b > b0 ? row[b - 1] : 0.0)) / m;
                prev = cPrime[b];
            }
            for (int b = b1 - 1; b >= b0; --b)
                row[b] -= cPrime[b] * row[b + 1];
            for (int b = b0; b <= b1; ++b)
            {
                const double e = std::min(std::max(row[b], 0.0), 1.0);
                out[at(a, b)] = static_cast<float>(std::min(cap * std::pow(e, 1.0 / kEnergyPower) + stepLength, cap));
            }
            b0 = b1 + 1;
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
    const double cap = std::max(b.fetch, 1.0);
    auto energy = [cap](float fetch) { return fetch >= cap ? 1.0 : std::pow(fetch / cap, kEnergyPower); };
    std::vector<float> radial;
    const double step = kWaterOpenRadialStep * kPi / 180.0;
    double wind = std::atan2(b.dirZ, b.dirX);
    if (b.automatic && wet)
    {
        // Each direction's mean energy over the water (every kSampleStride-th texel), then each
        // candidate's by the spreading
        const std::size_t kSampleStride = 7;
        double mean[kWindCandidates];
        for (int d = 0; d < kWindCandidates; ++d)
        {
            fetchSweep(water, open, cell, std::cos(d * step), std::sin(d * step), radial);
            double e = 0.0, n = 0.0;
            for (std::size_t k = 0; k < count; k += kSampleStride)
                if (water[k])
                {
                    e += energy(radial[k]);
                    n += 1.0;
                }
            mean[d] = n > 0.0 ? e / n : 0.0;
        }
        double best = -1.0;
        for (int c = 0; c < kWindCandidates; ++c)
        {
            double score = 0.0;
            for (int r = 0; r < kRadials; ++r)
                score += b.spreading[r] * mean[((c + r - (kRadials - 1) / 2) % kWindCandidates + kWindCandidates)
                                              % kWindCandidates];
            if (score > best + 1e-9)
            {
                best = score;
                wind = c * step;
            }
        }
        b.dirX = std::cos(wind);
        b.dirZ = std::sin(wind);
    }
    std::vector<double> sum(count, 0.0);
    double weights = 0.0;
    for (int r = 0; r < kRadials; ++r)
    {
        const double offset = (r - (kRadials - 1) / 2) * step;
        const double w = b.spreading[r];
        if (w <= 0.0)
            continue;
        fetchSweep(water, open, cell, std::cos(wind + offset), std::sin(wind + offset), radial, 0.25 * b.wavelength, cap);
        for (std::size_t k = 0; k < count; ++k)
            if (water[k])
                sum[k] += w * energy(radial[k]);
        weights += w;
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
            const double mean = weights > 0.0 ? sum[k] / weights : 0.0;
            const float fetch = static_cast<float>(cap * std::pow(mean, 1.0 / kEnergyPower));
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
    s.haveDir = false;
    s.mapNew = false;
    s.tableNew = s.haveTable;
}

void rendererWaterOpenTable(const float table[kWaterOpenTableRows][kWaterOpenTable][4],
                            const float spreading[kWaterOpenRadials], double fetch, double peakWavelength)
{
    OpenState& s = st();
    std::memcpy(s.table, table, sizeof(s.table));
    std::memcpy(s.spreading, spreading, sizeof(s.spreading));
    s.fetch = fetch;
    s.wavelength = peakWavelength;
    s.haveTable = true;
    s.tableNew = true;
}

bool rendererWaterOpenWind(double& x, double& z)
{
    OpenState& s = st();
    if (!s.haveDir)
        return false;
    x = s.info.dirX;
    z = s.info.dirZ;
    return true;
}

void rendererWaterOpenFrame(bool automatic, double windX, double windZ)
{
    OpenState& s = st();
    if (s.building && s.done)
    {
        joinBake(s);
        s.map.swap(s.bake.texels);
        std::vector<float>().swap(s.bake.heights);
        std::vector<float>().swap(s.bake.texels);
        s.info = s.bake;
        s.haveDir = true;
        s.mapNew = true;
        std::fprintf(stderr, "[WATER] open-water map: %d x %d over %.0f m, sea level %.2f m, wind toward "
                     "(%.2f, %.2f)%s, level fetch %.0f km: water %.1f %% of the map, %d edge runs open sea, mean effective fetch "
                     "%.0f m; baked in %.0f ms\n", kSide, kSide, 2.0 * s.info.half, s.info.sea,
                     s.info.dirX, s.info.dirZ, s.info.automatic ? " (from the open sea)" : "",
                     s.info.fetch / 1000.0, 100.0 * s.info.waterShare, s.info.openEdges,
                     std::exp2(s.info.meanLog2Fetch), s.info.ms);
    }

    // A new level, a new sea (the spreading and the level's fetch come with each spectrum) or the
    // wind turned (given, not automatic): sample the heights here, bake on a worker
    geom::Terrain* const t = geom::terrain();
    world::IHeightmapCluster* const cluster = heightmapCluster();
    if (t && cluster && cluster->getPrimaryHeightmap() && s.haveTable && !s.building && (s.pending || s.haveKey))
    {
        const float half = 0.5f * static_cast<float>(t->m_patchCount * t->m_patchSize) * t->m_primaryWorldScale.x;
        const float sea = cluster->getSeaWaterLevel();
        const bool changed = !s.haveKey || sea != s.keySea || half != s.keyHalf || automatic != s.keyAutomatic
                          || (!automatic && (windX != s.keyDirX || windZ != s.keyDirZ)) || s.fetch != s.keyFetch || s.wavelength != s.keyWavelength
                          || std::memcmp(s.spreading, s.keySpreading, sizeof(s.spreading)) != 0;
        if (changed && half > 0.0f)
        {
            s.pending = false;
            s.haveKey = true;
            s.keySea = sea;
            s.keyHalf = half;
            s.keyAutomatic = automatic;
            s.keyDirX = windX;
            s.keyDirZ = windZ;
            s.keyFetch = s.fetch;
            s.keyWavelength = s.wavelength;
            std::memcpy(s.keySpreading, s.spreading, sizeof(s.spreading));
            Bake& b = s.bake;
            b.sea = sea;
            b.half = half;
            b.automatic = automatic;
            b.dirX = windX;
            b.dirZ = windZ;
            b.fetch = s.fetch;
            b.wavelength = s.wavelength;
            std::memcpy(b.spreading, s.spreading, sizeof(s.spreading));
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

// [TOOLS] tools.water (render milestone 23): the open-water map's state and table to base.txt, its
// signed distance to base-distance.png (grey 0.5 the shore, +-256 m white and black) and its fetch
// to base-fetch.png (log2, 16 m black to 64 km white).
bool toolsWriteWater(const char* base, const char*& message)
{
    OpenState& s = st();
    message = "";
    if (!base || s.map.empty())
    {
        s.message = "the open-water map is not baked";
        message = s.message.c_str();
        return false;
    }
    const std::string root(base);
    FILE* const f = std::fopen((root + ".txt").c_str(), "w");
    if (!f)
    {
        s.message = "cannot write " + root + ".txt";
        message = s.message.c_str();
        return false;
    }
    std::fprintf(f, "# tools.water (render milestone 23): the open-water map and its fetch table\n");
    const double from = std::fmod(std::atan2(-s.info.dirX, -s.info.dirZ) * 180.0 / kPi + 360.0, 360.0);
    std::fprintf(f, "[map] %d x %d over %.1f m (%.2f m a texel), sea level %.2f m, wind toward (%.3f, %.3f) "
                    "from %.0f degrees (0 +z, 90 +x), %s, level fetch %.0f km, peak wavelength %.1f m (diffraction), bound %s\n", kSide, kSide,
                 2.0 * s.info.half, 2.0 * s.info.half / kSide, s.info.sea, s.info.dirX, s.info.dirZ, from,
                 s.info.automatic ? "from the open sea" : "renderer.waterWindFrom", s.info.fetch / 1000.0,
                 s.info.wavelength,
                 s.scale > 0.0f ? "yes" : "no");
    std::fprintf(f, "[spreading] by energy, -90 to +90 degrees:");
    for (int r = 0; r < kRadials; ++r)
        std::fprintf(f, " %.3f", s.info.spreading[r]);
    std::fprintf(f, "\n");
    std::fprintf(f, "[bake] water %.2f %% of the map, %d edge runs open sea, %.0f ms\n",
                 100.0 * s.info.waterShare, s.info.openEdges, s.info.ms);
    const std::size_t count = static_cast<std::size_t>(kSide) * kSide;
    double bins[6] = { 0, 0, 0, 0, 0, 0 };
    static const double kBinTop[6] = { 100.0, 1000.0, 5000.0, 20000.0, 100000.0, 1e30 };
    std::size_t wet = 0;
    for (std::size_t k = 0; k < count; ++k)
        if (s.map[k * 4] > 0.0f)
        {
            const double fetch = std::exp2(s.map[k * 4 + 3]);
            int bin = 0;
            while (fetch > kBinTop[bin])
                ++bin;
            bins[bin] += 1.0;
            ++wet;
        }
    std::fprintf(f, "[fetch] of the water: under 100 m %.1f %%, 100 m-1 km %.1f %%, 1-5 km %.1f %%, "
                    "5-20 km %.1f %%, 20-100 km %.1f %%, open sea %.1f %%\n",
                 wet ? 100.0 * bins[0] / wet : 0.0, wet ? 100.0 * bins[1] / wet : 0.0, wet ? 100.0 * bins[2] / wet : 0.0,
                 wet ? 100.0 * bins[3] / wet : 0.0, wet ? 100.0 * bins[4] / wet : 0.0, wet ? 100.0 * bins[5] / wet : 0.0);
    if (s.haveTable)
        for (int e = 0; e < kWaterOpenTable; ++e)
            std::fprintf(f, "[table %2d] fetch %7.0f m: amplitude %.3f %.3f %.3f, height x%.3f, whitecaps x%.3f\n", e,
                         std::exp2(static_cast<double>(kWaterOpenTableFirst + e)), s.table[0][e][0], s.table[0][e][1],
                         s.table[0][e][2], s.table[0][e][3], s.table[1][e][0]);
    else
        std::fprintf(f, "[table] not written yet\n");
    std::fclose(f);

    IDirect3DDevice9* const d = device();
    bool ok = d != 0;
    for (int image = 0; image < 2 && ok; ++image)
    {
        IDirect3DSurface9* surface = 0;
        ok = d->CreateOffscreenPlainSurface(kSide, kSide, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, 0) >= 0;
        D3DLOCKED_RECT locked;
        if (ok && surface->LockRect(&locked, 0, 0) >= 0)
        {
            for (int j = 0; j < kSide; ++j)
                for (int i = 0; i < kSide; ++i)
                {
                    const float* const p = &s.map[(static_cast<std::size_t>(j) * kSide + i) * 4];
                    const float t = image == 0 ? 0.5f + p[0] / 512.0f
                                  : p[0] > 0.0f ? (p[3] - 4.0f) / 12.0f : 0.0f;
                    const unsigned int g = static_cast<unsigned int>(std::min(std::max(t, 0.0f), 1.0f) * 255.0f);
                    // Rows from z high at the top: north up
                    reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits)
                        + static_cast<std::size_t>(kSide - 1 - j) * locked.Pitch)[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
                }
            surface->UnlockRect();
            ok = D3DXSaveSurfaceToFileA((root + (image == 0 ? "-distance.png" : "-fetch.png")).c_str(),
                                        D3DXIFF_PNG, surface, 0, 0) >= 0;
        }
        else
            ok = false;
        if (surface)
            surface->Release();
    }
    s.message = ok ? root + ".txt" : "could not write the images";
    message = s.message.c_str();
    return ok;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

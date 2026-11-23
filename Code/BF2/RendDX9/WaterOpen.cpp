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
// Beside it (columns kSide to 2 kSide - 1) the shore map (task 3, contract 40):
//
//   r   the depth under the sea level, metres (negative on land)
//   g   the phase of the level's peak wave refracted by the bed (shoreBake: an eikonal solve from the
//       deep water's plane wave), radians modulo kShorePhaseWrap; land up the beach continues it
//       (under kShoreLandHeight above the sea, the swash's reach), and the open-water map's fetch
//   b a its direction of travel, unit; zero where no train runs (higher land, water the march missed)
//
// Two rows below the map hold a table over log2 fetch, kTable entries from kTableFirst (one per
// octave), written by Water.cpp whenever the spectrum changes (rendererWaterOpenTable): row 0 each
// wave cascade's slope amplitude against the level's fetch and the significant height's ratio;
// row 1 the whitecap coverage's ratio. Row 0's texel kShoreConstants holds the shore train's angular
// frequency, its groups' (both whole cycles of the waves' loop) and the level's significant height.
// The texture is A16B16G16R16F, 2 kSide x (kSide + 2), managed,
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
#include "Texture.h"
#include "../Geom/Terrain.h"
#include "../Physics/HeightmapCluster.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

const int kSide = 1024;              // the map's texels per side
const int kTexWidth = 2 * kSide;     // the open-water map, then the shore map
const int kTexRows = kSide + 2;      // and the table's two rows
const int kShoreConstants = kWaterOpenTable;   // the shore train's constants: this column of the table's first row
const int kRadials = kWaterOpenRadials;   // about the wind, kWaterOpenRadialStep apart
const int kWindCandidates = 60;      // automatic wind directions, kWaterOpenRadialStep apart
const double kEnergyPower = 4.0 / 3.0 - 0.22;   // a fetch-limited JONSWAP sea's variance ~ F^this
const double kOpenRun = 1000.0;      // m of water along a map edge that continues as open sea past it
const float kOpenFetch = 1.0e6f;     // m: open sea's fetch, past renderer.waterFetch's range
const double kPi = 3.14159265358979323846;
const double kGravity = 9.81;
const double kShoreDeep = kPi;       // k0 d past which the bed is not felt (d > half the peak wavelength)
const double kShoreMinDepth = 0.2;   // m: the train's depth at the waterline (a bore's speed, not zero)
const double kShorePhaseWrap = 8.0 * kPi;   // the stored phase's period (16-bit floats)
const double kShoreGroupWaves = 7.0; // waves to a group
const float kShoreLandHeight = 4.0f; // m above the sea the train's phase is carried up the beach (the swash's reach)
const int kShoreLandRings = 128;     // and at most this many texels in from the water

struct Bake
{
    std::vector<float> heights;      // kSide^2, row j at z
    float sea, half;
    bool automatic;                  // choose the wind's direction (dirX, dirZ out) or take it (in)
    double dirX, dirZ;
    double fetch;                    // the level's, m
    double wavelength;               // its sea's peak, m
    double omega;                    // the shore train's angular frequency (the peak's, whole cycles of the loop)
    float spreading[kWaterOpenRadials];
    std::vector<float> texels;       // kSide^2 x 4, the map's r g b a
    std::vector<float> shore;        // kSide^2 x 4, the shore map's r g b a
    double deepShare;                // of the water, deep enough to start the train
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
    std::vector<float> shore;        // and its shore map
    Bake info;                       // its inputs and statistics (no arrays)
    bool mapNew;                     // not uploaded yet
    float table[kWaterOpenTableRows][kWaterOpenTable][4];
    float spreading[kWaterOpenRadials];
    double fetch, wavelength;
    double hs, omega, omegaGroup;    // the level's significant height, the train's frequencies
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

// The wavenumber of a wave of angular frequency omega over depth d (finite-depth dispersion,
// omega^2 = g k tanh(k d)): Fenton and McKee's approximation, then two Newton steps
double shoreWavenumber(double omega, double d)
{
    const double k0 = omega * omega / kGravity;
    if (k0 * d > 10.0)
        return k0;
    double k = k0 / std::pow(std::tanh(std::pow(k0 * d, 0.75)), 2.0 / 3.0);
    for (int n = 0; n < 2; ++n)
    {
        const double t = std::tanh(k * d);
        const double f = k * t - k0;
        const double df = t + k * d * (1.0 - t * t);
        k -= f / df;
    }
    return k;
}

// The shore map (task 3): per texel the depth under the sea level and the phase of the level's peak
// wave refracted by the bed. The phase solves the eikonal equation |grad phi| = k(d) (geometric
// optics: crests are its level sets, rays its gradient, so the waves shorten and turn toward the
// shore's normal as the water shallows) by fast marching (Sethian 1996, first order, four
// neighbours) over the water from the deep water (k0 d > kShoreDeep), where it is the open sea's
// plane wave from the wind, k0 (wind . x); a point is reached by its first arrival, round a
// breakwater's tip as by Huygens. On a map with no deep water (a river) the deepest half starts it.
// Water the march never reaches (a lake) and land have no direction: no train.
void shoreBake(Bake& b, const std::vector<unsigned char>& water, double cell)
{
    const std::size_t count = static_cast<std::size_t>(kSide) * kSide;
    const double omega = b.omega;
    const double k0 = omega * omega / kGravity;
    std::vector<double> k(count, 0.0), phi(count, 1e300);
    std::vector<unsigned char> known(count, 0);
    double deepest = 0.0;
    for (std::size_t c = 0; c < count; ++c)
        if (water[c])
            deepest = std::max(deepest, static_cast<double>(b.sea - b.heights[c]));
    const double start = std::min(kShoreDeep / k0, 0.5 * deepest);
    typedef std::pair<double, std::size_t> Entry;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry> > heap;
    std::size_t wet = 0, deep = 0;
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
        {
            const std::size_t c = static_cast<std::size_t>(j) * kSide + i;
            if (!water[c])
                continue;
            ++wet;
            const double d = b.sea - b.heights[c];
            k[c] = shoreWavenumber(omega, std::max(d, kShoreMinDepth));
            if (d >= start && start > 0.0)
            {
                const double x = -b.half + (i + 0.5) * cell, z = -b.half + (j + 0.5) * cell;
                phi[c] = k0 * (b.dirX * x + b.dirZ * z);
                known[c] = 1;
                ++deep;
            }
        }
    b.deepShare = wet ? static_cast<double>(deep) / wet : 0.0;
    // The march's front: the shallow neighbours of the deep water
    auto update = [&](int i, int j) {
        const std::size_t c = static_cast<std::size_t>(j) * kSide + i;
        if (!water[c] || known[c] == 1)
            return;
        double a = 1e300, bz = 1e300;
        if (i > 0 && known[c - 1] == 1) a = std::min(a, phi[c - 1]);
        if (i + 1 < kSide && known[c + 1] == 1) a = std::min(a, phi[c + 1]);
        if (j > 0 && known[c - kSide] == 1) bz = std::min(bz, phi[c - kSide]);
        if (j + 1 < kSide && known[c + kSide] == 1) bz = std::min(bz, phi[c + kSide]);
        const double f = k[c] * cell;
        double v;
        if (a > 1e299 && bz > 1e299)
            return;
        if (a > 1e299 || bz > 1e299 || std::fabs(a - bz) >= f)
            v = std::min(a, bz) + f;
        else
            v = 0.5 * (a + bz + std::sqrt(2.0 * f * f - (a - bz) * (a - bz)));
        if (v < phi[c])
        {
            phi[c] = v;
            known[c] = 2;
            heap.push(Entry(v, c));
        }
    };
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
            if (known[static_cast<std::size_t>(j) * kSide + i] == 1)
            {
                if (i > 0) update(i - 1, j);
                if (i + 1 < kSide) update(i + 1, j);
                if (j > 0) update(i, j - 1);
                if (j + 1 < kSide) update(i, j + 1);
            }
    while (!heap.empty())
    {
        const Entry e = heap.top();
        heap.pop();
        const std::size_t c = e.second;
        if (known[c] == 1 || e.first > phi[c])
            continue;
        known[c] = 1;
        const int i = static_cast<int>(c % kSide), j = static_cast<int>(c / kSide);
        if (i > 0) update(i - 1, j);
        if (i + 1 < kSide) update(i + 1, j);
        if (j > 0) update(i, j - 1);
        if (j + 1 < kSide) update(i, j + 1);
    }

    // The direction the waves travel: the phase's gradient over the reached water (one-sided at
    // the shore). Land next to reached water takes its phase, so the shader's filter across the
    // waterline stays continuous
    b.shore.assign(count * 4, 0.0f);
    std::vector<double> filled(phi);
    std::vector<unsigned char> has(count);
    std::vector<std::size_t> front, next;
    for (std::size_t c = 0; c < count; ++c)
    {
        has[c] = water[c] && known[c] == 1;
        if (has[c])
            front.push_back(c);
    }
    // Land up the beach (contract 41: the swash runs on the waterline's train) takes, ring by ring,
    // the mean phase of its neighbours already filled, and the open-water map's fetch with it,
    // while it lies under kShoreLandHeight above the sea, at most kShoreLandRings texels in
    std::vector<unsigned char> ring(count, 0);
    std::vector<std::vector<std::size_t> > rings;
    for (int r = 1; r <= kShoreLandRings && !front.empty(); ++r)
    {
        next.clear();
        for (std::size_t f = 0; f < front.size(); ++f)
        {
            const std::size_t c = front[f];
            const int i = static_cast<int>(c % kSide), j = static_cast<int>(c / kSide);
            const std::size_t around[4] = { i > 0 ? c - 1 : c, i + 1 < kSide ? c + 1 : c,
                                            j > 0 ? c - kSide : c, j + 1 < kSide ? c + kSide : c };
            for (int n = 0; n < 4; ++n)
            {
                const std::size_t a = around[n];
                if (has[a] || ring[a] || water[a] || b.heights[a] - b.sea > kShoreLandHeight)
                    continue;
                ring[a] = static_cast<unsigned char>(r);
                next.push_back(a);
            }
        }
        for (std::size_t f = 0; f < next.size(); ++f)
        {
            const std::size_t c = next[f];
            const int i = static_cast<int>(c % kSide), j = static_cast<int>(c / kSide);
            const std::size_t around[4] = { i > 0 ? c - 1 : c, i + 1 < kSide ? c + 1 : c,
                                            j > 0 ? c - kSide : c, j + 1 < kSide ? c + kSide : c };
            double sum = 0.0, fetch = 0.0, n = 0.0;
            for (int k = 0; k < 4; ++k)
                if (around[k] != c && has[around[k]])
                {
                    sum += filled[around[k]];
                    fetch += b.texels[around[k] * 4 + 3];
                    n += 1.0;
                }
            filled[c] = sum / n;   // a ring's texel touches the ring before it
            b.texels[c * 4 + 3] = static_cast<float>(fetch / n);
        }
        for (std::size_t f = 0; f < next.size(); ++f)
            has[next[f]] = 1;
        rings.push_back(next);
        front.swap(next);
    }
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
        {
            const std::size_t c = static_cast<std::size_t>(j) * kSide + i;
            float* const p = &b.shore[c * 4];
            p[0] = b.sea - b.heights[c];
            p[1] = has[c] ? static_cast<float>(filled[c] - kShorePhaseWrap * std::floor(filled[c] / kShorePhaseWrap)) : 0.0f;
            if (!water[c] || known[c] != 1)
                continue;
            auto reached = [&](std::size_t n) { return water[n] && known[n] == 1; };
            const bool l = i > 0 && reached(c - 1), r = i + 1 < kSide && reached(c + 1);
            const bool d = j > 0 && reached(c - kSide), u = j + 1 < kSide && reached(c + kSide);
            const double gx = (r ? phi[c + 1] : phi[c]) - (l ? phi[c - 1] : phi[c]);
            const double gz = (u ? phi[c + kSide] : phi[c]) - (d ? phi[c - kSide] : phi[c]);
            const double len = std::sqrt(gx * gx + gz * gz);
            if (len > 1e-9)
            {
                p[2] = static_cast<float>(gx / len);
                p[3] = static_cast<float>(gz / len);
            }
        }
    // The land that took a phase takes the mean direction of its neighbours a ring nearer the water
    // (the train runs to the waterline and the swash up the beach)
    for (std::size_t r = 0; r < rings.size(); ++r)
        for (std::size_t f = 0; f < rings[r].size(); ++f)
        {
            const std::size_t c = rings[r][f];
            const int i = static_cast<int>(c % kSide), j = static_cast<int>(c / kSide);
            const std::size_t around[4] = { i > 0 ? c - 1 : c, i + 1 < kSide ? c + 1 : c,
                                            j > 0 ? c - kSide : c, j + 1 < kSide ? c + kSide : c };
            float x = 0.0f, z = 0.0f;
            for (int n = 0; n < 4; ++n)
            {
                const std::size_t a = around[n];
                if (a != c && (water[a] || (ring[a] && ring[a] <= r)))   // ring r + 1 is this one's
                {
                    x += b.shore[a * 4 + 2];
                    z += b.shore[a * 4 + 3];
                }
            }
            const float len = std::sqrt(x * x + z * z);
            if (len > 1e-6f)
            {
                b.shore[c * 4 + 2] = x / len;
                b.shore[c * 4 + 3] = z / len;
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
    shoreBake(b, water, cell);
    b.ms = msSince(t0);
}

// ---- the texture ----

bool ensureTexture(OpenState& s)
{
    if (s.texture)
        return true;
    IDirect3DDevice9* const d = device();
    if (!d || d->CreateTexture(kTexWidth, kTexRows, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED, &s.texture, 0) < 0)
    {
        s.texture = 0;
        return false;
    }
    return true;
}

bool uploadRows(OpenState& s, int row0, int rows, int width, const float* texels, int x0 = 0)
{
    RECT r = { x0, row0, x0 + width, row0 + rows };
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
    std::vector<float>().swap(s.shore);
    std::vector<float>().swap(s.bake.shore);
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
                            const float spreading[kWaterOpenRadials], double fetch, double peakWavelength,
                            double significantHeight, double loopSeconds)
{
    OpenState& s = st();
    // The peak's frequency, and a group's (kShoreGroupWaves waves), each a whole number of cycles of
    // the loop so the train wraps with the spectrum
    const double peak = std::sqrt(2.0 * kPi * kGravity / std::max(peakWavelength, 0.1));
    const double cycle = 2.0 * kPi / loopSeconds;
    s.omega = std::max(std::floor(peak / cycle + 0.5), 1.0) * cycle;
    s.omegaGroup = std::max(std::floor(peak / (kShoreGroupWaves * cycle) + 0.5), 1.0) * cycle;
    s.hs = significantHeight;
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
        s.shore.swap(s.bake.shore);
        std::vector<float>().swap(s.bake.heights);
        std::vector<float>().swap(s.bake.texels);
        std::vector<float>().swap(s.bake.shore);
        s.info = s.bake;
        s.haveDir = true;
        s.mapNew = true;
        std::fprintf(stderr, "[WATER] open-water map: %d x %d over %.0f m, sea level %.2f m, wind toward "
                     "(%.2f, %.2f)%s, level fetch %.0f km: water %.1f %% of the map, %d edge runs open sea, mean effective fetch "
                     "%.0f m, %.0f %% of it deep enough to start the shore's waves; baked in %.0f ms\n", kSide, kSide, 2.0 * s.info.half, s.info.sea,
                     s.info.dirX, s.info.dirZ, s.info.automatic ? " (from the open sea)" : "",
                     s.info.fetch / 1000.0, 100.0 * s.info.waterShare, s.info.openEdges,
                     std::exp2(s.info.meanLog2Fetch), 100.0 * s.info.deepShare, s.info.ms);
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
            b.omega = s.omega;
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
            ok = uploadRows(s, 0, kSide, kSide, s.map.data()) && uploadRows(s, 0, kSide, kSide, s.shore.data(), kSide);
            s.mapNew = !ok;
            s.scale = ok ? 0.5f / s.info.half : 0.0f;
        }
        if (s.tableNew && s.haveTable && ok)
        {
            // The shore train's constants beside the table: its angular frequency and its groups'
            // (whole cycles of the waves' loop), the level's significant height
            const float constants[4] = { static_cast<float>(s.omega), static_cast<float>(s.omegaGroup),
                                         static_cast<float>(s.hs), 0.0f };
            s.tableNew = !(uploadRows(s, kSide, kWaterOpenTableRows, kWaterOpenTable, &s.table[0][0][0])
                           && uploadRows(s, kSide, 1, 1, constants, kShoreConstants));
        }
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

const void* rendererWaterShoreValue(float shore[4])
{
    static EngineTexture value;
    OpenState& s = st();
    float seconds = 0.0f;
    shore[0] = shore[1] = shore[2] = shore[3] = 0.0f;
    if (!g_waterOpen || !s.texture || s.scale <= 0.0f || !s.haveTable || s.tableNew
        || !rendererWaterLoopSeconds(seconds))
        return 0;
    value.baseTexture = s.texture;
    shore[0] = s.scale;
    shore[1] = seconds;
    shore[2] = s.info.sea;
    return &value;
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
    std::fprintf(f, "[shore] train period %.2f s (groups %.1f s), significant height %.2f m, deep water (starts it) "
                    "%.1f %% of the water\n", s.omega > 0.0 ? 2.0 * kPi / s.omega : 0.0,
                 s.omegaGroup > 0.0 ? 2.0 * kPi / s.omegaGroup : 0.0, s.hs, 100.0 * s.info.deepShare);
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
    for (int image = 0; image < 3 && ok && !(image == 2 && s.shore.empty()); ++image)
    {
        IDirect3DSurface9* surface = 0;
        ok = d->CreateOffscreenPlainSurface(kSide, kSide, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, 0) >= 0;
        D3DLOCKED_RECT locked;
        if (ok && surface->LockRect(&locked, 0, 0) >= 0)
        {
            for (int j = 0; j < kSide; ++j)
                for (int i = 0; i < kSide; ++i)
                {
                    const std::size_t c = (static_cast<std::size_t>(j) * kSide + i) * 4;
                    const float* const p = &s.map[c];
                    const float* const q = image == 2 ? &s.shore[c] : p;
                    // The shore map: the crests (cos of the phase) where the train runs, dimmer the
                    // deeper (to half the peak wavelength), land black
                    const float t = image == 0 ? 0.5f + p[0] / 512.0f
                                  : image == 1 ? (p[0] > 0.0f ? (p[3] - 4.0f) / 12.0f : 0.0f)
                                  : (q[0] > 0.0f && (q[2] != 0.0f || q[3] != 0.0f))
                                    ? (0.5f + 0.5f * std::cos(q[1])) * (1.0f - 0.75f * std::min(q[0] / static_cast<float>(0.5 * s.info.wavelength), 1.0f))
                                    : 0.0f;
                    const unsigned int g = static_cast<unsigned int>(std::min(std::max(t, 0.0f), 1.0f) * 255.0f);
                    // Rows from z high at the top: north up
                    reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits)
                        + static_cast<std::size_t>(kSide - 1 - j) * locked.Pitch)[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
                }
            surface->UnlockRect();
            ok = D3DXSaveSurfaceToFileA((root + (image == 0 ? "-distance.png" : image == 1 ? "-fetch.png" : "-shore.png")).c_str(),
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

// [RENDER] Atmosphere (render milestone 11, docs/lanes/render.md#11-atmosphere-sky-ibl-water-reflections).
// Not part of retail BF2.
//
// Two layers. The sky atmosphere (Rayleigh, Mie and ozone over a planet of Earth's size, in
// kilometres) drives the sky; a ground haze (density falling exponentially with height, in metres)
// drives aerial perspective. At load the model is fitted to the level's authored light:
//   1. sunlight above the atmosphere = the sun at the ground over the sky's transmittance at the
//      sun's elevation, clamped to >= 0.1. The sun at the ground is the one every lit surface
//      takes: the authored sun (Lightmanager.staticSunColor) decoded as the shaders do,
//      colour^2.2 x 2^2.2 (gamma and the x2 overbright), as irradiance / pi;
//   2. the sky: Earth's air (a clear sky on every level; overcast is milestone 12's clouds),
//      physical under that sun. The authored sky and fog colours set no brightness (owner,
//      2026-11-21: physically correct, the look is adjusted in post);
//   3. the haze: density from fog end; its full in-scatter is the sky's radiance in each direction
//      (the horizon's at and below it), so terrain at fog end meets the sky without a seam.
// The transmittance, multiple-scattering and sky-view tables are built here on the CPU after a fit
// (the sun does not move; about 100 ms with the fit) and uploaded to managed textures, so they are
// the very model the fit evaluated and survive device resets. Only the aerial-perspective atlas is
// drawn per frame, by RaAtmosphere.fx (shader contracts 6 and 8, renderer-pipeline.md), and bound
// to the reserved sampler s9 for every lit shader (Ra_GetAerial, with RaAerial in c223). Pixel passes for
// the load-time tables were tried first: this D3DX compiler's loops that are not unrolled came out
// wrong under DXVK (the multiple-scattering table 10x low), so the atlas's march is unrolled too.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX          // std::min and std::max below
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "Texture.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Mat4.h"
#include "../Math/Vec3.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/QuadPatch.h"
#include "../Geom/SkyDome.h"
#include "../GeomMisc/FSQuadDrawer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

namespace geom {
extern LightManager* g_LightManager;   // ModuleGeom.cpp
}

namespace dx9rend {

extern void* g_rendererBase;
extern void* g_shaderManager;
void* effectD3dxHandle(void* effect);   // Effect.cpp

namespace {

// ---- the model (mirrors RaAtmosphere.fx) ----

const double kPi = 3.14159265358979;
// 2^2.2: the authored x2 overbright, decoded with the colour's gamma (RA_LIGHTMAP_X2 in the shaders)
const double kSunOverbright = 4.5948;
const double kGroundRadius = 6360.0;   // km
const double kTopRadius = 6460.0;      // km
const double kRayleighHeight = 8.0;    // km
const double kMieHeight = 1.2;         // km
const double kOzoneCentre = 25.0;      // km
const double kOzoneHalfWidth = 15.0;   // km
const double kEarthRayleigh[3] = { 5.802e-3, 13.558e-3, 33.1e-3 };   // 1/km
const double kMieScattering = 3.996e-3;                              // 1/km at density 1
const double kMieAbsorption = 4.40e-3;                               // 1/km at density 1
const double kMieG = 0.8;
const double kOzone[3] = { 0.650e-3, 1.881e-3, 0.085e-3 };           // 1/km

// The haze layer: one shape for every level, set by renderer.hazeDepth and renderer.hazeFalloff
// (tuned at the aerial-perspective A/B, task 4); the density follows fog end. It stays exponential
// past fog end: the view distance is cut by the radial dither (task 5).

// Sizes, as RaAtmosphere.fx defines them.
const unsigned int kTransmittanceW = 256, kTransmittanceH = 64;
const unsigned int kMultiScatterSize = 32;
const unsigned int kSkyViewW = 192, kSkyViewH = 108;
const unsigned int kApTiles = 32, kApSlices = 32;
// The atlas's last block (contract 20): the haze's light per tile column and elevation, so
// Ra_GetAerial lights each pixel by its own ray's elevation, not its tile's.
const unsigned int kApBlocks = kApSlices + 1;

// The sky (task 3): the model's clear sky and the sun disc.
const double kSunAngularRadius = 0.27;      // degrees, the real sun's
const double kSunDiscEdge = 0.15;           // the disc's soft edge, as a share of its radius
const double kSunDiscMaxLuminance = 100.0;  // scene units (the physical disc is ~46000x the sun colour)

struct V3
{
    double x, y, z;
};
inline V3 v3(double x, double y, double z) { V3 v = { x, y, z }; return v; }
inline V3 operator+(const V3& a, const V3& b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(const V3& a, const V3& b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(const V3& a, const V3& b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline V3 operator*(const V3& a, double s) { return v3(a.x * s, a.y * s, a.z * s); }
inline V3 operator/(const V3& a, const V3& b) { return v3(a.x / b.x, a.y / b.y, a.z / b.z); }
inline double dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 vexp(const V3& a) { return v3(std::exp(a.x), std::exp(a.y), std::exp(a.z)); }
inline V3 vmax(const V3& a, double m) { return v3(std::max(a.x, m), std::max(a.y, m), std::max(a.z, m)); }
inline double at(const V3& a, int i) { return i == 0 ? a.x : i == 1 ? a.y : a.z; }

inline double srgbToLinear(double c)
{
    c = std::min(std::max(c, 0.0), 1.0);
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

struct Media
{
    V3 rayleigh, mie, extinction;
};

// The fitted sky: Rayleigh scattering (1/km) and the Mie density (1 = Earth's).
struct SkyParams
{
    V3 rayleigh;
    double mie;
    V3 groundAlbedo;
};

Media media(const SkyParams& p, double h)
{
    const double dr = std::exp(-h / kRayleighHeight);
    const double dm = std::exp(-h / kMieHeight);
    const double dozone = std::max(0.0, 1.0 - std::fabs(h - kOzoneCentre) / kOzoneHalfWidth);
    Media m;
    m.rayleigh = p.rayleigh * dr;
    const double mie = p.mie * kMieScattering * dm;
    m.mie = v3(mie, mie, mie);
    const double mieExt = p.mie * (kMieScattering + kMieAbsorption) * dm;
    m.extinction = m.rayleigh + v3(mieExt, mieExt, mieExt)
                 + v3(kOzone[0], kOzone[1], kOzone[2]) * dozone;
    return m;
}

double raySphere(double r, double mu, double radius)
{
    const double disc = r * r * (mu * mu - 1.0) + radius * radius;
    if (disc < 0.0)
        return -1.0;
    const double s = std::sqrt(disc);
    const double farT = -r * mu + s, nearT = -r * mu - s;
    if (farT < 0.0)
        return -1.0;
    return nearT >= 0.0 ? nearT : farT;
}

bool hitsGround(double r, double mu)
{
    return mu < 0.0 && r * r * (mu * mu - 1.0) + kGroundRadius * kGroundRadius >= 0.0;
}

double rayleighPhase(double c) { return 3.0 / (16.0 * kPi) * (1.0 + c * c); }
double miePhase(double g, double c)
{
    const double k = 3.0 / (8.0 * kPi) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / std::pow(std::max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// Transmittance to the top, marched (no ground test).
V3 transmittanceMarch(const SkyParams& p, double r, double mu, int steps)
{
    const double len = std::max(raySphere(r, mu, kTopRadius), 0.0);
    const double dt = len / steps;
    V3 depth = v3(0, 0, 0);
    for (int i = 0; i < steps; ++i)
    {
        const double t = (i + 0.5) * dt;
        const double ri = std::sqrt(r * r + t * t + 2.0 * r * mu * t);
        depth = depth + media(p, ri - kGroundRadius).extinction * dt;
    }
    return vexp(depth * -1.0);
}

// The tables. The fit evaluates the model on coarse ones; the frame's are the same functions at
// full size. Texel i of n holds unit coordinate i / (n - 1): a texel centre, as the shaders'
// UvFromUnit reads it.
// - Transmittance (kTransmittanceW x kTransmittanceH): Bruneton's parameterization; x the
//   distance to the top along the ray between its least and greatest values at that radius,
//   y the radius by the horizon distance (0 ground, 1 top).
// - Multiple scattering (kMultiScatterSize square): x the sun cosine (0 = -1, 1 = +1), y the
//   altitude (0 ground, 1 top); Hillaire's L2 / (1 - fms) for unit sunlight.
const int kFitTransW = 64, kFitTransH = 32;
const int kFitMsSize = 16;

struct CpuTables
{
    int tw, th, msn;
    std::vector<V3> trans;   // [y * tw + x]
    std::vector<V3> ms;      // [y * msn + x]
};

void transUnitToRMu(double ux, double uy, double& r, double& mu)
{
    const double H = std::sqrt(kTopRadius * kTopRadius - kGroundRadius * kGroundRadius);
    const double rho = H * uy;
    r = std::sqrt(rho * rho + kGroundRadius * kGroundRadius);
    const double dMin = kTopRadius - r, dMax = rho + H;
    const double d = dMin + ux * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = std::min(std::max(mu, -1.0), 1.0);
}

void transRMuToUnit(double r, double mu, double& ux, double& uy)
{
    const double H = std::sqrt(kTopRadius * kTopRadius - kGroundRadius * kGroundRadius);
    const double rho = std::sqrt(std::max(r * r - kGroundRadius * kGroundRadius, 0.0));
    const double disc = r * r * (mu * mu - 1.0) + kTopRadius * kTopRadius;
    const double d = std::max(0.0, -r * mu + std::sqrt(std::max(disc, 0.0)));
    const double dMin = kTopRadius - r, dMax = rho + H;
    ux = (d - dMin) / std::max(dMax - dMin, 1e-9);
    uy = rho / H;
}

V3 bilinear(const std::vector<V3>& t, int w, int h, double ux, double uy)
{
    const double fx = std::min(std::max(ux, 0.0), 1.0) * (w - 1);
    const double fy = std::min(std::max(uy, 0.0), 1.0) * (h - 1);
    const int x0 = std::min(static_cast<int>(fx), w - 2), y0 = std::min(static_cast<int>(fy), h - 2);
    const double ax = fx - x0, ay = fy - y0;
    const V3 a = t[y0 * w + x0] * (1 - ax) + t[y0 * w + x0 + 1] * ax;
    const V3 b = t[(y0 + 1) * w + x0] * (1 - ax) + t[(y0 + 1) * w + x0 + 1] * ax;
    return a * (1 - ay) + b * ay;
}

V3 transmittance(const CpuTables& c, double r, double mu)
{
    if (hitsGround(r, mu))
        return v3(0, 0, 0);
    double ux, uy;
    transRMuToUnit(r, mu, ux, uy);
    return bilinear(c.trans, c.tw, c.th, ux, uy);
}

V3 multiScatter(const CpuTables& c, double r, double muS)
{
    return bilinear(c.ms, c.msn, c.msn, muS * 0.5 + 0.5,
                    (r - kGroundRadius) / (kTopRadius - kGroundRadius));
}

void buildTransmittance(const SkyParams& p, CpuTables& c, int w, int h)
{
    c.tw = w;
    c.th = h;
    c.trans.resize(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            double r, mu;
            transUnitToRMu(static_cast<double>(x) / (w - 1), static_cast<double>(y) / (h - 1), r, mu);
            c.trans[y * w + x] = transmittanceMarch(p, r, mu, 40);
        }
}

// Hillaire 2020, section 5.5: for a point lit by unit sunlight, the second-order light arriving
// from 64 directions under an isotropic phase (L2) and the share the medium around it scatters
// back (fms); the series sums to L2 / (1 - fms). Its sky contribution is that times the medium's
// scattering.
void buildMultiScatter(const SkyParams& p, CpuTables& c, int n)
{
    const int dirs = 8, steps = 20;
    c.msn = n;
    c.ms.resize(n * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const double muS = static_cast<double>(x) / (n - 1) * 2.0 - 1.0;
            double r = kGroundRadius + static_cast<double>(y) / (n - 1) * (kTopRadius - kGroundRadius);
            r = std::min(std::max(r, kGroundRadius + 0.01), kTopRadius - 0.01);
            const V3 sun = v3(std::sqrt(std::max(0.0, 1.0 - muS * muS)), muS, 0.0);
            const V3 pos = v3(0.0, r, 0.0);
            V3 l2 = v3(0, 0, 0), fms = v3(0, 0, 0);
            for (int j = 0; j < dirs; ++j)
                for (int i = 0; i < dirs; ++i)
                {
                    const double theta = 2.0 * kPi * (i + 0.5) / dirs;
                    const double cosPhi = 1.0 - 2.0 * (j + 0.5) / dirs;
                    const double sinPhi = std::sqrt(std::max(0.0, 1.0 - cosPhi * cosPhi));
                    const V3 dir = v3(std::cos(theta) * sinPhi, cosPhi, std::sin(theta) * sinPhi);
                    const bool ground = hitsGround(r, dir.y);
                    const double len = ground ? raySphere(r, dir.y, kGroundRadius)
                                              : std::max(raySphere(r, dir.y, kTopRadius), 0.0);
                    const double dt = len / steps;
                    V3 thr = v3(1, 1, 1), l = v3(0, 0, 0), f = v3(0, 0, 0);
                    for (int s = 0; s < steps; ++s)
                    {
                        const V3 q = pos + dir * ((s + 0.5) * dt);
                        const double rs = length(q);
                        const Media m = media(p, rs - kGroundRadius);
                        const V3 scat = m.rayleigh + m.mie;
                        const V3 stepT = vexp(m.extinction * -dt);
                        const V3 ext = vmax(m.extinction, 1e-9);
                        const V3 sunT = transmittance(c, rs, dot(q, sun) / rs);
                        const V3 src = scat * sunT * (1.0 / (4.0 * kPi));
                        l = l + thr * (src - src * stepT) / ext;
                        f = f + thr * (scat - scat * stepT) / ext;
                        thr = thr * stepT;
                    }
                    if (ground)
                    {
                        const V3 q = pos + dir * len;
                        const double rs = length(q);
                        const double muG = dot(q, sun) / rs;
                        l = l + thr * transmittance(c, rs, muG) * p.groundAlbedo
                                * (std::max(muG, 0.0) / kPi);
                    }
                    l2 = l2 + l;
                    fms = fms + f;
                }
            const double samples = dirs * dirs;
            l2 = l2 * (1.0 / samples);
            fms = fms * (1.0 / samples);
            c.ms[y * n + x] = v3(l2.x / (1.0 - std::min(fms.x, 0.999)),
                                          l2.y / (1.0 - std::min(fms.y, 0.999)),
                                          l2.z / (1.0 - std::min(fms.z, 0.999)));
        }
}

// The sky's in-scatter along a ray from radius r (the frame's y the local up there), for unit
// sunlight above the atmosphere: single scattering plus the multiple-scattering table.
V3 skyInScatter(const SkyParams& p, const CpuTables& c, double r, const V3& dir, const V3& sun,
                int steps)
{
    const bool ground = hitsGround(r, dir.y);
    const double len = ground ? raySphere(r, dir.y, kGroundRadius)
                              : std::max(raySphere(r, dir.y, kTopRadius), 0.0);
    const double dt = len / steps;
    const double cosTheta = dot(dir, sun);
    const double pr = rayleighPhase(cosTheta), pm = miePhase(kMieG, cosTheta);
    const V3 pos = v3(0.0, r, 0.0);
    V3 thr = v3(1, 1, 1), l = v3(0, 0, 0);
    for (int s = 0; s < steps; ++s)
    {
        const V3 q = pos + dir * ((s + 0.5) * dt);
        const double rs = length(q);
        const double muS = dot(q, sun) / rs;
        const Media m = media(p, rs - kGroundRadius);
        const V3 stepT = vexp(m.extinction * -dt);
        const V3 single = (m.rayleigh * pr + m.mie * pm) * transmittance(c, rs, muS);
        const V3 multi = (m.rayleigh + m.mie) * multiScatter(c, rs, muS);
        const V3 src = single + multi;
        l = l + thr * (src - src * stepT) / vmax(m.extinction, 1e-9);
        thr = thr * stepT;
    }
    return l;
}

// Sky view (kSkyViewW x kSkyViewH; Hillaire 2020, section 5.3), seen from radius r: x the azimuth
// from the sun, squared (0 toward it, 1 away); y the zenith angle, squared toward the horizon from
// both sides (0 zenith, 0.5 horizon, 1 nadir); texel (x, y) at ((x + 0.5) / w, (y + 0.5) / h).
// Radiance: the sky's in-scatter times the sunlight above the atmosphere.
void buildSkyView(const SkyParams& p, const CpuTables& c, double r, const V3& sun, const V3& scale,
                  int w, int h, std::vector<V3>& out)
{
    out.resize(w * h);
    const double beta = std::acos(std::sqrt(std::max(r * r - kGroundRadius * kGroundRadius, 0.0)) / r);
    const double zenithHorizon = kPi - beta;
    for (int y = 0; y < h; ++y)
    {
        const double v = (y + 0.5) / h;
        double zenith;
        if (v < 0.5)
        {
            const double k = 1.0 - 2.0 * v;
            zenith = zenithHorizon * (1.0 - k * k);
        }
        else
        {
            const double k = 2.0 * v - 1.0;
            zenith = zenithHorizon + beta * k * k;
        }
        for (int x = 0; x < w; ++x)
        {
            const double u = (x + 0.5) / w;
            const double cosAz = -(u * u * 2.0 - 1.0);
            const double sinAz = std::sqrt(std::max(0.0, 1.0 - cosAz * cosAz));
            const V3 dir = v3(std::sin(zenith) * cosAz, std::cos(zenith), std::sin(zenith) * sinAz);
            out[y * w + x] = skyInScatter(p, c, r, dir, sun, 30) * scale;
        }
    }
}

// ---- cloud light (render milestone 12, task 1) ----
// The light a cloud receives at its altitude and position, from this model (after Gaijin's Dagor
// daSkies2, `clouds_light_color`; BSD-3-Clause): the clouds' include (RaCloud.fxh) reads it per
// sample. One 2D texture of kCloudLightLayers tiles across (altitudes above the reference height,
// layer i at (i + 0.5) / layers of kCloudLightTopKm) by three rows of tiles down: the sun's
// irradiance there, the sky's mean radiance from above (its irradiance / pi), and the ground's
// radiance from below. A tile's texel (x, z) sits at world x, z = ((i + 0.5) / tile * 2 - 1) times
// kCloudLightRangeKm from the level's origin: the planet's curvature lowers the sun far away
// (sunset on the distant deck first). The haze is left out (its density at 1.5 km is e^-4 of the
// ground's), and the ground everywhere has the level's albedo.
const int kCloudLightLayers = 8, kCloudLightTile = 8, kCloudLightRows = 3;
const double kCloudLightTopKm = 12.0, kCloudLightRangeKm = 280.0;
const int kCloudLightMuS = 33;   // sun cosines -0.32 .. 1 for the irradiance

// The sky's irradiance from the upper hemisphere at radius r, for unit sunlight above the
// atmosphere: cosine-weighted rings, half the azimuths (the sky is symmetric about the sun's plane).
V3 skyIrradiance(const SkyParams& p, const CpuTables& c, double r, double muS)
{
    const int rings = 8, azimuths = 8;
    const V3 sun = v3(std::sqrt(std::max(0.0, 1.0 - muS * muS)), muS, 0.0);
    V3 sum = v3(0, 0, 0);
    for (int i = 0; i < rings; ++i)
    {
        const double ct = std::sqrt((i + 0.5) / rings), st = std::sqrt(1.0 - ct * ct);
        for (int j = 0; j < azimuths; ++j)
        {
            const double a = kPi * (j + 0.5) / azimuths;
            sum = sum + skyInScatter(p, c, r, v3(st * std::cos(a), ct, st * std::sin(a)), sun, 20);
        }
    }
    return sum * (kPi / (rings * azimuths));
}

double cloudLightMuS(int i) { return -0.32 + 1.32 * i / (kCloudLightMuS - 1); }

V3 lookupMuS(const std::vector<V3>& t, double muS)
{
    const double f = std::min(std::max((muS + 0.32) / 1.32, 0.0), 1.0) * (kCloudLightMuS - 1);
    const int i = std::min(static_cast<int>(f), kCloudLightMuS - 2);
    return t[i] * (1.0 - (f - i)) + t[i + 1] * (f - i);
}

void buildCloudLight(const SkyParams& p, const CpuTables& c, double refHeightM, const V3& toSun,
                     const V3& sunTop, std::vector<V3>& out)
{
    const int w = kCloudLightLayers * kCloudLightTile, h = kCloudLightRows * kCloudLightTile;
    out.assign(w * h, v3(0, 0, 0));
    const double rg = kGroundRadius + std::max(refHeightM, 1.0) / 1000.0;
    std::vector<V3> groundSky(kCloudLightMuS), layerSky(kCloudLightMuS);
    for (int i = 0; i < kCloudLightMuS; ++i)
        groundSky[i] = skyIrradiance(p, c, rg, cloudLightMuS(i));
    for (int l = 0; l < kCloudLightLayers; ++l)
    {
        const double r = rg + kCloudLightTopKm * (l + 0.5) / kCloudLightLayers;
        for (int i = 0; i < kCloudLightMuS; ++i)
            layerSky[i] = skyIrradiance(p, c, r, cloudLightMuS(i));
        // The air between the ground and this altitude, straight up.
        const V3 airUp = transmittance(c, rg, 1.0) / vmax(transmittance(c, r, 1.0), 1e-9);
        for (int tz = 0; tz < kCloudLightTile; ++tz)
            for (int tx = 0; tx < kCloudLightTile; ++tx)
            {
                const double x = ((tx + 0.5) / kCloudLightTile * 2.0 - 1.0) * kCloudLightRangeKm;
                const double z = ((tz + 0.5) / kCloudLightTile * 2.0 - 1.0) * kCloudLightRangeKm;
                const V3 up = v3(x, rg, z) * (1.0 / length(v3(x, rg, z)));
                const double muS = std::min(std::max(dot(up, toSun), -1.0), 1.0);
                const V3 sunGround = sunTop * transmittance(c, rg, muS);
                const V3 groundIrr = sunGround * std::max(muS, 0.0) + lookupMuS(groundSky, muS) * sunTop;
                const int x0 = l * kCloudLightTile + tx;
                out[(0 * kCloudLightTile + tz) * w + x0] = sunTop * transmittance(c, r, muS);
                out[(1 * kCloudLightTile + tz) * w + x0] = lookupMuS(layerSky, muS) * sunTop * (1.0 / kPi);
                out[(2 * kCloudLightTile + tz) * w + x0] = p.groundAlbedo * groundIrr * airUp * (1.0 / kPi);
            }
    }
}

// ---- inputs and the fit ----

struct Inputs
{
    const void* terrain;
    float toSun[3];         // unit, toward the sun
    float sun[3];           // Lightmanager.staticSunColor
    float sky[3];           // Lightmanager.staticSkyColor
    float fog[3];           // 0-255
    float fogEnd;           // m
};

bool sameInputs(const Inputs& a, const Inputs& b)
{
    return a.terrain == b.terrain && a.fogEnd == b.fogEnd
        && std::memcmp(a.toSun, b.toSun, sizeof(a.toSun)) == 0
        && std::memcmp(a.sun, b.sun, sizeof(a.sun)) == 0
        && std::memcmp(a.sky, b.sky, sizeof(a.sky)) == 0
        && std::memcmp(a.fog, b.fog, sizeof(a.fog)) == 0;
}

struct State
{
    bool haveInputs;
    Inputs inputs;
    // Read once per texture or terrain.
    const void* terrainRead;
    double refHeight;        // m: the terrain's mean height
    V3 groundAlbedo;         // linear: the colormaps' mean colour
    // The fit.
    bool fitted;
    SkyParams sky;
    V3 sunTop;               // irradiance above the atmosphere (pi * scene sun units)
    V3 zenithModel, horizonModel, horizonTarget;
    double skyGain;          // horizon luminance: authored over the model's
    CpuTables tables;        // full size, for the textures
    std::vector<V3> cloudLightData;   // buildCloudLight
    double fitMs;
    // GPU.
    IDirect3DTexture9* trans;
    IDirect3DTexture9* ms;
    IDirect3DTexture9* skyView;
    IDirect3DTexture9* cloudLightTexture;
    IDirect3DTexture9* atlas;
    D3DFORMAT atlasFormat;
    bool formatChecked;
    bool atlasVertexFetch;
    bool atlasVertexFilter;
    bool tablesDirty;        // the fit changed: upload the transmittance and multiple scattering
    bool skyViewDirty;       // the fit or the blend changed: rebuild and upload the sky view
    bool cloudLightDirty;    // the fit changed: upload the cloud light
    bool effectMissing;
    bool atlasLive;          // this frame's atlas is drawn and bound to s9
    float cutRange;          // rendererViewDistanceCutRange
    // Render milestone 12 task 7: the clouded sky from the ground (rendererAtmosphereCloudSky).
    std::vector<float> skyViewRgb;   // the last sky view, for the sky cube
    std::vector<float> cloudSky;     // rgba, decoded; empty: clear
    unsigned int cloudSkyW, cloudSkyH;
    bool cubeDirty;          // the clouds changed: rebuild the sky cube from the same sky view
};

State g = {};
float g_hazeDepth = 1.0f;      // renderer.hazeDepth: horizontal optical depth at fog end
float g_hazeFalloff = 400.0f;  // renderer.hazeFalloff: m, the haze's density falls by e over this height

// 1/m at the reference height.
float hazeDensity()
{
    return g.inputs.fogEnd > 0.0f ? g_hazeDepth / g.inputs.fogEnd : 0.0f;
}

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

// The surfaces' sun from an authored channel: Ra_SunIrradiance in the shaders (gamma 2.2, the x2
// overbright decoded with it).
float surfaceSun(float authored)
{
    return static_cast<float>(std::pow(std::max(static_cast<double>(authored), 0.0), 2.2) * kSunOverbright);
}

bool gatherInputs(Inputs& in)
{
    geom::LightManager* const lm = geom::g_LightManager;
    geom::SkyDome* const sky = static_cast<geom::SkyDome*>(geom::g_SkyDome);
    const float* const range = reinterpret_cast<const float*>(raShaderConstantsAt(0xA8));
    if (!lm || !sky || !range || range[0] == 0.0f)
        return false;
    std::memset(&in, 0, sizeof(in));
    in.terrain = geom::terrain();
    const Vec3 d = lm->getSunDirection();
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len <= 0.0f)
        return false;
    in.toSun[0] = -d.x / len;
    in.toSun[1] = -d.y / len;
    in.toSun[2] = -d.z / len;
    const Vec3& sun = lm->getStaticSunColor();
    const Vec3& skyColor = lm->getStaticSkyColor();
    in.sun[0] = surfaceSun(sun.x); in.sun[1] = surfaceSun(sun.y); in.sun[2] = surfaceSun(sun.z);
    in.sky[0] = skyColor.x; in.sky[1] = skyColor.y; in.sky[2] = skyColor.z;
    postProductionGetFogColor255(in.fog);
    in.fogEnd = range[3] / range[0];
    return in.fogEnd > 1.0f;
}

// The terrain's mean height (the patches' mid-heights: some levels' file heights read 0) and its
// colormaps' mean colour.
void readTerrain(const void* terrainPtr)
{
    g.terrainRead = terrainPtr;
    g.refHeight = 0.0;
    g.groundAlbedo = v3(0.2, 0.2, 0.2);
    geom::Terrain* const t = static_cast<geom::Terrain*>(const_cast<void*>(terrainPtr));
    if (!t || !t->m_patches)
        return;
    double heights = 0.0, colour[3] = {};
    int patches = 0, read = 0;
    std::string why;
    for (int i = 0; i < t->m_patchCount * t->m_patchCount; ++i)
    {
        geom::QuadPatch* const patch = t->m_patches[i];
        if (!patch)
            continue;
        heights += 0.5 * (patch->minHeight + patch->maxHeight);
        ++patches;
        double mean[4];
        if (patch->colormap
            && textureMeanColour(static_cast<const EngineTexture*>(patch->colormap), 0, 16, mean, why))
        {
            for (int k = 0; k < 3; ++k)
                colour[k] += mean[k];
            ++read;
        }
    }
    if (patches)
        g.refHeight = heights / patches;
    if (read)
        g.groundAlbedo = v3(srgbToLinear(colour[0] / read / 255.0),
                            srgbToLinear(colour[1] / read / 255.0),
                            srgbToLinear(colour[2] / read / 255.0));
}

double luminance(const V3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

struct FitContext
{
    double r;            // km: the radius the sky is seen from
    double muS;          // the sun's cosine from the zenith
    V3 toSun;            // local frame: y up, the sun in the x-y plane
    V3 sunGround;        // the authored sun
    V3 sunTop;           // from the last evaluate
    double horizonElevation;
    CpuTables tables;    // the transmittance per evaluation; multiple scattering per round
};

// 1. Sunlight above the atmosphere: the authored sun over the transmittance at the sun's
// elevation, clamped to >= 0.1 per channel (low suns, night layers).
V3 sunAbove(const SkyParams& p, const FitContext& f)
{
    const V3 t = transmittanceMarch(p, f.r, f.muS, 64);
    return v3(kPi * f.sunGround.x / std::max(t.x, 0.1), kPi * f.sunGround.y / std::max(t.y, 0.1),
              kPi * f.sunGround.z / std::max(t.z, 0.1));
}

// The zenith and the azimuth-averaged horizon, with the parameters' own
// sunlight above the atmosphere and transmittance, and the context's multiple scattering.
void evaluate(const SkyParams& p, FitContext& f, V3& zenith, V3& horizon)
{
    f.sunTop = sunAbove(p, f);
    buildTransmittance(p, f.tables, kFitTransW, kFitTransH);
    zenith = skyInScatter(p, f.tables, f.r, v3(0, 1, 0), f.toSun, 30) * f.sunTop;
    const int azimuths = 8;
    const double ce = std::cos(f.horizonElevation), se = std::sin(f.horizonElevation);
    V3 sum = v3(0, 0, 0);
    for (int i = 0; i < azimuths; ++i)
    {
        const double a = 2.0 * kPi * (i + 0.5) / azimuths;
        sum = sum + skyInScatter(p, f.tables, f.r, v3(ce * std::cos(a), se, ce * std::sin(a)),
                                 f.toSun, 30);
    }
    horizon = sum * (1.0 / azimuths) * f.sunTop;
}

// 2. The sky: Earth's air on every level, a clear sky (owner, 2026-11-20). The authored domes'
// colours are mostly painted cloud and haze; fitted to them, the Rayleigh tint and density greyed
// Kashan's sky and turned overcast Saaremaa's navy. Overcast is milestone 12's clouds. Nothing
// else is fitted: the sky is physical under the surfaces' sun (owner, 2026-11-21).
SkyParams earthSky(const V3& albedo)
{
    SkyParams p;
    p.rayleigh = v3(kEarthRayleigh[0], kEarthRayleigh[1], kEarthRayleigh[2]);
    p.mie = 1.0;
    p.groundAlbedo = albedo;
    return p;
}

void fit()
{
    const LARGE_INTEGER zero = {};
    LARGE_INTEGER t0 = zero, t1 = zero, freq = zero;
    QueryPerformanceCounter(&t0);
    const Inputs& in = g.inputs;
    const double elevation = std::asin(std::min(std::max(static_cast<double>(in.toSun[1]), -1.0), 1.0));

    FitContext f;
    f.r = kGroundRadius + std::max(g.refHeight, 1.0) / 1000.0;
    f.muS = std::max(std::sin(elevation), 0.02);
    f.toSun = v3(std::sqrt(1.0 - f.muS * f.muS), f.muS, 0.0);
    f.sunGround = v3(in.sun[0], in.sun[1], in.sun[2]);
    f.horizonElevation = 1.0 * kPi / 180.0;
    const V3 horizonTarget = vmax(v3(srgbToLinear(in.fog[0] / 255.0), srgbToLinear(in.fog[1] / 255.0),
                                     srgbToLinear(in.fog[2] / 255.0)), 1e-5);

    g.sky = earthSky(g.groundAlbedo);
    buildTransmittance(g.sky, f.tables, kFitTransW, kFitTransH);
    buildMultiScatter(g.sky, f.tables, kFitMsSize);
    evaluate(g.sky, f, g.zenithModel, g.horizonModel);
    g.sunTop = f.sunTop;
    g.horizonTarget = horizonTarget;
    // The authored horizon over the model's, for the tools: not applied.
    g.skyGain = luminance(horizonTarget) / std::max(luminance(g.horizonModel), 1e-9);

    // 3. The haze: its density follows fog end each frame (hazeDensity); its light is the sky's
    //    (RaAtmosphere.fx).

    // The frame's tables at full size (the sky view follows in rendererAtmosphereFrame).
    buildTransmittance(g.sky, g.tables, kTransmittanceW, kTransmittanceH);
    buildMultiScatter(g.sky, g.tables, kMultiScatterSize);
    buildCloudLight(g.sky, g.tables, g.refHeight,
                    v3(in.toSun[0], in.toSun[1], in.toSun[2]), g.sunTop, g.cloudLightData);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g.fitMs = freq.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart : 0.0;
    g.fitted = true;
    g.tablesDirty = true;
    g.skyViewDirty = true;
    g.cloudLightDirty = true;

    std::fprintf(stderr,
        "[ATMOS] fit and tables %.0f ms: sun elevation %.1f deg, reference height %.1f m, fog end %.0f m\n"
        "[ATMOS]   authored horizon / model %.2f (not applied)\n"
        "[ATMOS]   sun ground %.3f %.3f %.3f -> above %.3f %.3f %.3f (irradiance)\n"
        "[ATMOS]   zenith model %.4f %.4f %.4f\n"
        "[ATMOS]   horizon target %.4f %.4f %.4f model %.4f %.4f %.4f\n"
        "[ATMOS]   ground albedo %.3f %.3f %.3f\n",
        g.fitMs, elevation * 180.0 / kPi, g.refHeight, in.fogEnd,
        g.skyGain,
        f.sunGround.x, f.sunGround.y, f.sunGround.z, g.sunTop.x, g.sunTop.y, g.sunTop.z,
        g.zenithModel.x, g.zenithModel.y, g.zenithModel.z,
        horizonTarget.x, horizonTarget.y, horizonTarget.z,
        g.horizonModel.x, g.horizonModel.y, g.horizonModel.z,
        g.groundAlbedo.x, g.groundAlbedo.y, g.groundAlbedo.z);
}

// ---- GPU ----

void releaseTexture(IDirect3DTexture9*& t)
{
    if (t)
        t->Release();
    t = 0;
}

// The atlas is read per vertex by particles (task 4): it must allow vertex texture fetch.
void checkAtlasFormat()
{
    g.formatChecked = true;
    g.atlasFormat = D3DFMT_A16B16G16R16F;
    IDirect3D9* d3d = 0;
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DDISPLAYMODE mode;
    if (device()->GetDirect3D(&d3d) < 0 || !d3d)
        return;
    if (device()->GetCreationParameters(&cp) >= 0 && device()->GetDisplayMode(0, &mode) >= 0)
    {
        static const D3DFORMAT kFormats[2] = { D3DFMT_A16B16G16R16F, D3DFMT_A32B32G32R32F };
        for (int i = 0; i < 2; ++i)
        {
            const bool fetch = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_VERTEXTEXTURE, D3DRTYPE_TEXTURE, kFormats[i]) >= 0;
            const bool filter = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_VERTEXTEXTURE | D3DUSAGE_QUERY_FILTER,
                D3DRTYPE_TEXTURE, kFormats[i]) >= 0;
            std::fprintf(stderr, "[ATMOS] atlas format %s: vertex fetch %s, filtered %s\n",
                         i == 0 ? "A16B16G16R16F" : "A32B32G32R32F", fetch ? "yes" : "no",
                         filter ? "yes" : "no");
            if (fetch)
            {
                g.atlasFormat = kFormats[i];
                g.atlasVertexFetch = true;
                g.atlasVertexFilter = filter;
                break;
            }
        }
    }
    d3d->Release();
}

unsigned short floatToHalf(float f)
{
    unsigned int bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const unsigned int sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    const unsigned int mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return static_cast<unsigned short>(sign);                     // flush to zero
    if (exponent >= 31)
        return static_cast<unsigned short>(sign | 0x7BFFu);           // clamp to the largest
    // Round to nearest.
    unsigned int h = sign | (static_cast<unsigned int>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u)
        ++h;
    return static_cast<unsigned short>(h);
}

// A table into its managed A16B16G16R16F texture (created on first use; managed textures survive
// device resets).
bool uploadTable(IDirect3DTexture9*& texture, const std::vector<V3>& data, unsigned int w, unsigned int h)
{
    if (!texture && device()->CreateTexture(w, h, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_MANAGED,
                                            &texture, 0) < 0)
    {
        std::fprintf(stderr, "[ATMOS] table %ux%u not created\n", w, h);
        texture = 0;
        return false;
    }
    D3DLOCKED_RECT lr;
    if (texture->LockRect(0, &lr, 0, 0) < 0)
        return false;
    for (unsigned int y = 0; y < h; ++y)
    {
        unsigned short* const row = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch);
        for (unsigned int x = 0; x < w; ++x)
        {
            const V3& v = data[y * w + x];
            row[x * 4 + 0] = floatToHalf(static_cast<float>(v.x));
            row[x * 4 + 1] = floatToHalf(static_cast<float>(v.y));
            row[x * 4 + 2] = floatToHalf(static_cast<float>(v.z));
            row[x * 4 + 3] = floatToHalf(1.0f);
        }
    }
    texture->UnlockRect(0);
    return true;
}

void updateTables()
{
    if (g.tablesDirty)
    {
        g.tablesDirty = !(uploadTable(g.trans, g.tables.trans, kTransmittanceW, kTransmittanceH)
                          && uploadTable(g.ms, g.tables.ms, kMultiScatterSize, kMultiScatterSize));
    }
    if (g.cloudLightDirty)
        g.cloudLightDirty = !uploadTable(g.cloudLightTexture, g.cloudLightData,
                                         kCloudLightLayers * kCloudLightTile, kCloudLightRows * kCloudLightTile);
    if (g.skyViewDirty)
    {
        const double elevation = std::asin(std::min(std::max(static_cast<double>(g.inputs.toSun[1]), -1.0), 1.0));
        const double muS = std::sin(elevation);
        std::vector<V3> sky;
        buildSkyView(g.sky, g.tables, kGroundRadius + std::max(g.refHeight, 1.0) / 1000.0,
                     v3(std::sqrt(std::max(0.0, 1.0 - muS * muS)), muS, 0.0), g.sunTop,
                     kSkyViewW, kSkyViewH, sky);
        g.skyViewDirty = !uploadTable(g.skyView, sky, kSkyViewW, kSkyViewH);
        // The sky cube and irradiance (task 7) from this sky view.
        g.skyViewRgb.resize(sky.size() * 3);
        for (size_t i = 0; i < sky.size(); ++i)
        {
            g.skyViewRgb[i * 3 + 0] = static_cast<float>(sky[i].x);
            g.skyViewRgb[i * 3 + 1] = static_cast<float>(sky[i].y);
            g.skyViewRgb[i * 3 + 2] = static_cast<float>(sky[i].z);
        }
        g.cubeDirty = true;
    }
    if (g.cubeDirty && !g.skyViewRgb.empty())
    {
        g.cubeDirty = false;
        SkyCubeSource src;
        src.skyView = &g.skyViewRgb[0];
        src.skyViewW = kSkyViewW;
        src.skyViewH = kSkyViewH;
        src.groundRadiusKm = kGroundRadius;
        src.altitudeKm = std::max(g.refHeight, 1.0) / 1000.0;
        for (int k = 0; k < 3; ++k)
        {
            src.toSun[k] = g.inputs.toSun[k];
            src.sun[k] = g.inputs.sun[k];
            src.groundAlbedo[k] = static_cast<float>(at(g.sky.groundAlbedo, k));
        }
        src.hazeDensity = hazeDensity();
        // Milestone 12 task 7: with the clouds as the ground sees them.
        src.clouds = g.cloudSky.empty() ? 0 : &g.cloudSky[0];
        src.cloudsW = g.cloudSkyW;
        src.cloudsH = g.cloudSkyH;
        rendererSkyCubeBuild(src);
    }
}

bool ensureAtlas()
{
    if (!g.formatChecked)
        checkAtlasFormat();
    if (g.atlas)
        return true;
    const HRESULT hr = device()->CreateTexture(kApTiles * kApBlocks, kApTiles, 1, D3DUSAGE_RENDERTARGET,
                                               g.atlasFormat, D3DPOOL_DEFAULT, &g.atlas, 0);
    if (hr < 0)
    {
        std::fprintf(stderr, "[ATMOS] atlas failed: hr=0x%08lX\n", static_cast<unsigned long>(hr));
        g.atlas = 0;
        return false;
    }
    return true;
}

// RaAtmosphere's 16 float4s (layout in RaAtmosphere.fx).
void fillConstants(float c[16][4])
{
    std::memset(c, 0, sizeof(float) * 64);
    const Inputs& in = g.inputs;
    const SkyParams& p = g.sky;
    const float mieS = static_cast<float>(p.mie * kMieScattering);
    const float mieE = static_cast<float>(p.mie * (kMieScattering + kMieAbsorption));
    const float rows[11][4] = {
        { float(p.rayleigh.x), float(p.rayleigh.y), float(p.rayleigh.z), float(kGroundRadius) },
        { mieS, mieS, mieS, float(kTopRadius) },
        { mieE, mieE, mieE, float(kMieG) },
        { float(kOzone[0]), float(kOzone[1]), float(kOzone[2]), float(std::max(g.refHeight, 1.0) / 1000.0) },
        { float(g.sunTop.x), float(g.sunTop.y), float(g.sunTop.z),
          1.0f },
        { in.toSun[0], in.toSun[1], in.toSun[2], 0.0f },
        { float(p.groundAlbedo.x), float(p.groundAlbedo.y), float(p.groundAlbedo.z), 0.0f },
        { hazeDensity(), 1.0f / g_hazeFalloff, float(g.refHeight), in.fogEnd },
        { 0.0f, 0.0f, 0.0f, 1.0f },   // the air's light under the clouds, below
        { in.sun[0], in.sun[1], in.sun[2], 0.0f },
        { in.sky[0], in.sky[1], in.sky[2], 0.0f },
    };
    std::memcpy(c, rows, sizeof(rows));
    // Render milestone 12 task 7: the air under the clouds (Ra_HazeLight, RaAtmosphere.fxh).
    if (!g.cloudSky.empty())
    {
        const SkyCubeCloudLight l = rendererSkyCubeCloudLight();
        c[8][0] = l.hazeLight[0];
        c[8][1] = l.hazeLight[1];
        c[8][2] = l.hazeLight[2];
        c[8][3] = l.sunTransmittance;
    }
}

// Rows 11-14: the camera and the view rays at unit view depth through the screen's corners.
void fillCamera(float c[16][4])
{
    IRenderView* const view = rendererGetRenderView();
    const float* const m = view->getViewMatrixPtr()->m;
    const float* const proj = view->getProjectionMatrixPtr()->m;
    const float sx = 1.0f / proj[0], sy = 1.0f / proj[5];
    for (int k = 0; k < 3; ++k)
    {
        c[11][k] = m[12 + k];
        // top-left: forward - right * sx + up * sy
        c[12][k] = m[8 + k] - m[0 + k] * sx + m[4 + k] * sy;
        c[13][k] = 2.0f * m[0 + k] * sx;
        c[14][k] = -2.0f * m[4 + k] * sy;
    }
}

struct PassTarget
{
    IDirect3DSurface9* savedTarget;
    IDirect3DSurface9* savedDepth;
    D3DVIEWPORT9 savedViewport;
};

void beginPasses(PassTarget& s)
{
    s.savedTarget = stateManagerGetRenderTarget(0);
    s.savedDepth = stateManagerGetDepthStencilSurface();
    if (!s.savedTarget)
        s.savedTarget = rendererSceneColorSurface();
    if (s.savedTarget)
        s.savedTarget->AddRef();
    if (s.savedDepth)
        s.savedDepth->AddRef();
    device()->GetViewport(&s.savedViewport);
    stateManagerPush();
    stateManagerSetDepthStencilSurface(0);
}

void endPasses(PassTarget& s)
{
    stateManagerPop();
    stateManagerSetRenderTarget(0, s.savedTarget);
    stateManagerSetDepthStencilSurface(s.savedDepth);
    flushRenderTargetsSeam();
    device()->SetViewport(&s.savedViewport);
    if (s.savedTarget)
        s.savedTarget->Release();
    if (s.savedDepth)
        s.savedDepth->Release();
}

bool drawPass(void* effect, const std::string& technique, IDirect3DTexture9* target, float c[16][4])
{
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    D3DSURFACE_DESC desc;
    if (!d3dx || target->GetLevelDesc(0, &desc) < 0 || !effectSetTechniqueByName(effect, technique))
        return false;
    c[15][0] = static_cast<float>(desc.Width);
    c[15][1] = static_cast<float>(desc.Height);
    c[15][2] = 1.0f / c[15][0];
    c[15][3] = 1.0f / c[15][1];
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaAtmosphere"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), 16);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaSkyViewTable"))
        d3dx->SetTexture(h, g.skyView);

    IDirect3DSurface9* surface = 0;
    target->GetSurfaceLevel(0, &surface);
    stateManagerSetRenderTarget(0, surface);
    flushRenderTargetsSeam();
    const D3DVIEWPORT9 viewport = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
    device()->SetViewport(&viewport);
    effectBegin(effect);
    effectBeginPass(effect, 0);
    geom::fsQuadDrawerDrawCurrentEffect();
    effectEnd(effect);
    if (surface)
        surface->Release();
    return true;
}

// The atlas on the reserved sampler s9 (Ra_GetAerial), filtered within a slice; 0 unbinds it.
void bindAtlas(IDirect3DTexture9* atlas)
{
    IDirect3DDevice9* const d = device();
    d->SetTexture(kAerialSampler, atlas);
    if (!atlas)
        return;
    d->SetSamplerState(kAerialSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kAerialSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kAerialSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(kAerialSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kAerialSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kAerialSampler, D3DSAMP_SRGBTEXTURE, FALSE);
}

} // anonymous namespace

float& rendererHazeDepth()
{
    return g_hazeDepth;
}

float& rendererHazeFalloff()
{
    return g_hazeFalloff;
}

float rendererViewDistanceCutRange()
{
    return g.cutRange;
}

float rendererSceneryDistance()
{
    return 40000.0f;
}

void rendererAtmosphereFrame()
{
    g.atlasLive = false;
    if (!device())
        return;
    struct BindSkyCube   // task 7: at every return below, while the model draws the sky
    {
        ~BindSkyCube() { rendererSkyCubeFrame(rendererAtmosphereDrawsSky()); }
    } bindSkyCube;
    Inputs in;
    if (!gatherInputs(in))
        return;
    // Under water the fog constants are the water's: keep the last fit.
    if ((!g.haveInputs || !sameInputs(in, g.inputs)) && !cameraIsUnderWater())
    {
        g.inputs = in;
        g.haveInputs = true;
        if (in.terrain != g.terrainRead || !g.fitted)
            readTerrain(in.terrain);
        fit();
    }
    if (!g.fitted)
        return;
    updateTables();
    if (!rendererAtmosphereDrawsSky() || !ensureAtlas())   // the atlas's light is the sky view's
        return;

    void* const effect = shaderManagerGetEffect(g_shaderManager, "Shaders/RaAtmosphere");
    if (!effect || !effectD3dxHandle(effect))
    {
        if (!g.effectMissing)
            std::fprintf(stderr, "[ATMOS] Shaders/RaAtmosphere not loaded: no aerial-perspective atlas\n");
        g.effectMissing = true;
        if (effect)
            effectRelease(static_cast<Effect*>(effect));
        return;
    }
    g.effectMissing = false;

    float c[16][4];
    fillConstants(c);
    fillCamera(c);
    PassTarget saved;
    bindAtlas(0);   // never sampled while it is the target
    beginPasses(saved);
    static const std::string kAp("RaAtmosPerspective");
    g.atlasLive = drawPass(effect, kAp, g.atlas, c);
    endPasses(saved);
    effectRelease(static_cast<Effect*>(effect));
    if (g.atlasLive)
        bindAtlas(g.atlas);
}

void rendererAtmosphereAerialConstants(float c[8])
{
    for (int i = 0; i < 8; ++i)
        c[i] = 0.0f;
    g.cutRange = 0.0f;
    // The atlas's tiles span the world view's viewport; Ra_GetAerial takes it at the target's origin.
    const float* const vp = rendererGetRenderView()->getViewport();   // x, y, width, height
    if (!g.atlasLive || cameraIsUnderWater() || g.inputs.fogEnd <= 0.0f || vp[0] != 0.0f || vp[1] != 0.0f
        || vp[2] <= 0.0f || vp[3] <= 0.0f)
        return;
    c[0] = 1.0f / vp[2];
    c[1] = 1.0f / vp[3];
    c[2] = 1.0f / g.inputs.fogEnd;
    c[3] = 2.0f;   // 2: Ra_ViewDistanceClip cuts at fog end
    g.cutRange = g.inputs.fogEnd;
    // RaAerialHaze: the atlas march's density (RaAtmosphere.fx, its exponent capped at 30) at the
    // camera's height, for the closed-form haze past fog end.
    const double cameraY = rendererGetRenderView()->getViewMatrixPtr()->m[13];
    const double falloffs = std::min(-(cameraY - g.refHeight) / g_hazeFalloff, 30.0);
    c[4] = static_cast<float>(hazeDensity() * std::exp(falloffs));
    c[5] = 1.0f / g_hazeFalloff;
}

void rendererAtmosphereCloudSky(const float* rgba, unsigned int w, unsigned int h)
{
    if (rgba && w && h)
    {
        g.cloudSky.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
        g.cloudSkyW = w;
        g.cloudSkyH = h;
    }
    else
    {
        if (g.cloudSky.empty())
            return;
        std::vector<float>().swap(g.cloudSky);
        g.cloudSkyW = g.cloudSkyH = 0;
    }
    g.cubeDirty = true;
}

void rendererAtmosphereRelease()
{
    if (g.atlas && device())
        bindAtlas(0);
    g.atlasLive = false;
    releaseTexture(g.atlas);   // the tables are managed: they survive the reset
}

AtmosphereInfo rendererAtmosphereInfo()
{
    AtmosphereInfo i;
    std::memset(&i, 0, sizeof(i));
    i.fitted = g.fitted;
    i.transmittance = g.trans;
    i.multiScatter = g.ms;
    i.skyView = g.skyView;
    i.cloudLight = g.cloudLightTexture;
    i.perspective = g.atlas;
    i.fitMs = static_cast<float>(g.fitMs);
    i.refHeight = static_cast<float>(g.refHeight);
    i.fogEnd = g.inputs.fogEnd;
    i.skyGain = static_cast<float>(g.skyGain);
    i.hazeDensity = hazeDensity();
    for (int k = 0; k < 3; ++k)
    {
        i.toSun[k] = g.inputs.toSun[k];
        i.sunTop[k] = static_cast<float>(at(g.sunTop, k));
        i.zenithModel[k] = static_cast<float>(at(g.zenithModel, k));
        i.horizonTarget[k] = static_cast<float>(at(g.horizonTarget, k));
        i.horizonModel[k] = static_cast<float>(at(g.horizonModel, k));
        i.groundAlbedo[k] = static_cast<float>(at(g.sky.groundAlbedo, k));
    }
    i.atlasVertexFetch = g.atlasVertexFetch;
    i.atlasVertexFilter = g.atlasVertexFilter;
    i.atlasFloat32 = g.atlasFormat == D3DFMT_A32B32G32R32F;
    return i;
}

void rendererAtmosphereConstants(float c[16][4])
{
    fillConstants(c);
}

bool rendererAtmosphereDrawsSky()
{
    return g.fitted && g.skyView && !g.skyViewDirty;
}

// The model's sky (contract 7, SkyDome.fx technique RaSky): one full-screen pass at the far plane
// on the sky dome's effect, in the bound target and viewport, testing the scene depth without
// writing it. Returns false, drawing nothing, while the model does not draw the sky
// (no fit yet).
bool rendererAtmosphereDrawSky(void* effect, bool underWater)
{
    if (!rendererAtmosphereDrawsSky())
        return false;
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    static const std::string kSky("RaSky");
    if (!d3dx || !effectSetTechniqueByName(effect, kSky))
        return false;
    const D3DXHANDLE skyHandle = d3dx->GetParameterBySemantic(0, "RaSky");
    if (!skyHandle)
        return false;

    float c[16][4];
    fillConstants(c);
    fillCamera(c);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaAtmosphere"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), 16);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaSkyViewTable"))
        d3dx->SetTexture(h, g.skyView);

    // The sun disc: the authored sun (the sunlight at the ground, which the fit keeps) spread over
    // the disc's solid angle, so the disc's irradiance is the sun's; its luminance capped so the
    // MSAA resolve of its edge stays within a pixel (bloom, milestone 13, spreads it).
    const double radius = kSunAngularRadius * kPi / 180.0;
    const double solidAngle = 2.0 * kPi * (1.0 - std::cos(radius));
    V3 disc = v3(g.inputs.sun[0], g.inputs.sun[1], g.inputs.sun[2]) * (kPi / solidAngle);
    const double lum = luminance(disc);
    if (lum > kSunDiscMaxLuminance)
        disc = disc * (kSunDiscMaxLuminance / lum);
    const float* const vp = rendererGetRenderView()->getViewport();   // x, y, width, height
    const float sky[3][4] = {
        { underWater ? 1.0f : 0.0f, 0.0f, static_cast<float>(std::cos(radius)),
          static_cast<float>(std::cos(radius * (1.0 + kSunDiscEdge))) },
        { static_cast<float>(disc.x), static_cast<float>(disc.y), static_cast<float>(disc.z), 0.0f },
        { vp[0], vp[1], vp[2] > 0.0f ? 1.0f / vp[2] : 0.0f, vp[3] > 0.0f ? 1.0f / vp[3] : 0.0f },
    };
    d3dx->SetVectorArray(skyHandle, reinterpret_cast<const D3DXVECTOR4*>(sky), 3);
    rendererCloudsBindSky(d3dx);   // milestone 12: the cloud panoramas in front of it

    stateManagerPush();
    effectBegin(effect);
    effectBeginPass(effect, 0);
    geom::fsQuadDrawerDrawCurrentEffect();
    effectEnd(effect);
    stateManagerPop();
    return true;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

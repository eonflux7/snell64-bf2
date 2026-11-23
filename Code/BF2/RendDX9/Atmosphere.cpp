// [RENDER] Atmosphere (render milestones 11 and 18, docs/lanes/render.md#18-atmosphere-rework).
// Not part of retail BF2.
//
// One atmosphere over a planet of Earth's size (kilometres; radius 6360 km at sea level): Rayleigh
// and ozone at Earth's densities, and an aerosol set by the weather (renderer.weather*): its
// extinction at the level's ground from the visibility (Koschmieder, 3.912 / V, less the
// molecules'), near uniform up to the mixing layer's top and falling at 1.2 km above it; its type
// sets the single-scattering albedo, the phase asymmetry (Cornette-Shanks) and the Angstrom
// exponent (extinction ~ wavelength^-alpha: the haze's colour). The level's ground sits at its
// altitude (the weather's, else its far-ground bake's, else world y 0 at sea level).
//
// The sun: the authored sun (Lightmanager.staticSunColor, decoded as the shaders do: colour^2.2
// x 2^2.2) is the sun at the ground through a fixed reference air (fair: 25 km, continental,
// 1.5 km mixing layer, at the level's altitude). Sunlight above the atmosphere = the authored sun
// over the reference's transmittance at the sun's elevation (clamped >= 0.1: low suns, night
// layers); the surfaces' sun is that through this weather's air: the authored sun times the
// factor (this weather's transmittance over the reference's), which the shaders take from
// RaSunFactor (Ra_SunIrradiance). At the reference the lightmaps' anchor holds exactly.
//
// Tables (transmittance, multiple scattering, cloud light, the sky view at the ground for the sky
// cube) are built on the CPU on a worker thread when the weather or the authored light changes; the
// last ones stay bound meanwhile. The sky view the camera sees is built at the camera's height in
// 200 m bands (a jet above the mixing layer sees the clear sky over a haze layer). The
// aerial-perspective atlas is drawn per frame by RaAtmosphere.fx (contract 27) and bound to s9 for
// every lit shader (Ra_GetAerial). Pixel passes for the load-time tables were tried first: this
// D3DX compiler's loops that are not unrolled came out wrong under DXVK (the multiple-scattering
// table 10x low), so the atlas's march is unrolled too.
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
#include "../Geom/FarGround.h"
#include "../GeomMisc/FSQuadDrawer.h"

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
const double kGroundRadius = 6360.0;   // km: sea level
const double kTopRadius = 6460.0;      // km
const double kRayleighHeight = 8.0;    // km
const double kAerosolHeight = 1.2;     // km: the aerosol's scale height above the mixing layer
const double kOzoneCentre = 25.0;      // km
const double kOzoneHalfWidth = 15.0;   // km
const double kEarthRayleigh[3] = { 5.802e-3, 13.558e-3, 33.1e-3 };   // 1/km at sea level
const double kOzone[3] = { 0.650e-3, 1.881e-3, 0.085e-3 };           // 1/km
const double kWavelength[3] = { 680.0, 550.0, 440.0 };               // nm, the channels'
const double kMinAerosol = 2.0e-3;     // 1/km at 550 nm: the cleanest air keeps a trace

// Aerosol types (renderer.weatherAerosol): single-scattering albedo per channel, phase asymmetry,
// Angstrom exponent.
struct AerosolType
{
    const char* name;
    double albedo[3];
    double g;
    double alpha;
};
const AerosolType kAerosols[4] = {
    { "continental", { 0.90, 0.90, 0.90 }, 0.75, 1.3 },   // grey, faintly blue far off
    { "maritime", { 0.98, 0.98, 0.98 }, 0.80, 0.5 },      // white
    { "dust", { 0.96, 0.91, 0.80 }, 0.70, 0.2 },          // yellow-brown: blue absorbed
    { "smoke", { 0.85, 0.85, 0.85 }, 0.65, 1.8 },         // brown-grey, a red sun
};
const int kAerosolCount = 4;

// The reference air the sun is fitted against.
const double kRefVisibility = 25000.0, kRefMixing = 1500.0;
const int kRefAerosol = 0;

// Sizes, as RaAtmosphere.fx defines them.
const unsigned int kTransmittanceW = 256, kTransmittanceH = 64;
const unsigned int kMultiScatterSize = 32;
const unsigned int kSkyViewW = 192, kSkyViewH = 108;
// The atlas (contract 27): 32 in-scatter slices, 32 transmittance slices, the sky block.
const unsigned int kApTiles = 32, kApSlices = 32;
const unsigned int kApBlocks = 2 * kApSlices + 1;
const double kSkyBandKm = 0.2;          // the camera's sky view: rebuilt per band of height
const double kSkyBandSlackKm = 0.15;    // from the band's height before it moves (hysteresis)

// The sky (milestone 11, task 3): the sun disc.
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

// The air: Earth's molecules and this weather's aerosol.
struct SkyParams
{
    V3 rayleigh;             // 1/km at sea level
    V3 aerosolScattering;    // 1/km in the mixing layer
    V3 aerosolExtinction;    // 1/km in the mixing layer
    double g;                // the aerosol's phase asymmetry
    double groundKm;         // the level's ground above sea level
    double mixingTopKm;      // the mixing layer's top above sea level
    V3 groundAlbedo;
};

// The air for a visibility (m) at the level's ground, an aerosol type and a mixing height (m).
SkyParams makeAir(double visibilityM, int aerosol, double mixingM, double groundKm, const V3& albedo)
{
    const AerosolType& type = kAerosols[std::min(std::max(aerosol, 0), kAerosolCount - 1)];
    SkyParams p;
    p.rayleigh = v3(kEarthRayleigh[0], kEarthRayleigh[1], kEarthRayleigh[2]);
    p.groundKm = std::max(groundKm, 0.0);
    p.mixingTopKm = p.groundKm + std::max(mixingM, 50.0) / 1000.0;
    const double molecules = kEarthRayleigh[1] * std::exp(-p.groundKm / kRayleighHeight);
    const double ext550 = std::max(3912.0 / std::max(visibilityM, 100.0) - molecules, kMinAerosol);
    double ext[3], scat[3];
    for (int k = 0; k < 3; ++k)
    {
        ext[k] = ext550 * std::pow(kWavelength[k] / 550.0, -type.alpha);
        scat[k] = ext[k] * type.albedo[k];
    }
    p.aerosolExtinction = v3(ext[0], ext[1], ext[2]);
    p.aerosolScattering = v3(scat[0], scat[1], scat[2]);
    p.g = type.g;
    p.groundAlbedo = albedo;
    return p;
}

// The aerosol's density (1 in the mixing layer) at h km above sea level.
double aerosolDensity(const SkyParams& p, double h)
{
    return h <= p.mixingTopKm ? 1.0 : std::exp(-(h - p.mixingTopKm) / kAerosolHeight);
}

Media media(const SkyParams& p, double h)
{
    const double dr = std::exp(-h / kRayleighHeight);
    const double da = aerosolDensity(p, h);
    const double dozone = std::max(0.0, 1.0 - std::fabs(h - kOzoneCentre) / kOzoneHalfWidth);
    Media m;
    m.rayleigh = p.rayleigh * dr;
    m.mie = p.aerosolScattering * da;
    m.extinction = m.rayleigh + p.aerosolExtinction * da + v3(kOzone[0], kOzone[1], kOzone[2]) * dozone;
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
// Cornette-Shanks.
double miePhase(double g, double c)
{
    const double k = 3.0 / (8.0 * kPi) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + c * c) / std::pow(std::max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5);
}

// Transmittance to the top, marched (no ground test). The steps are finer near the start, where
// the aerosol is.
V3 transmittanceMarch(const SkyParams& p, double r, double mu, int steps)
{
    const double len = std::max(raySphere(r, mu, kTopRadius), 0.0);
    V3 depth = v3(0, 0, 0);
    double prev = 0.0;
    for (int i = 0; i < steps; ++i)
    {
        const double u = static_cast<double>(i + 1) / steps;
        const double t1 = len * u * u;
        const double t = 0.5 * (prev + t1);
        const double ri = std::sqrt(r * r + t * t + 2.0 * r * mu * t);
        depth = depth + media(p, ri - kGroundRadius).extinction * (t1 - prev);
        prev = t1;
    }
    return vexp(depth * -1.0);
}

// The tables. Texel i of n holds unit coordinate i / (n - 1): a texel centre, as the shaders'
// UvFromUnit reads it.
// - Transmittance (kTransmittanceW x kTransmittanceH): Bruneton's parameterization; x the
//   distance to the top along the ray between its least and greatest values at that radius,
//   y the radius by the horizon distance (0 ground, 1 top).
// - Multiple scattering (kMultiScatterSize square): x the sun cosine (0 = -1, 1 = +1), y the
//   altitude (0 ground, 1 top); Hillaire's L2 / (1 - fms) for unit sunlight.
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
// scattering. Isotropic: it runs dark at a vertical aerosol depth above about 1.
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
                    V3 thr = v3(1, 1, 1), l = v3(0, 0, 0), f = v3(0, 0, 0);
                    double prev = 0.0;
                    for (int s = 0; s < steps; ++s)
                    {
                        // Finer steps near the point, where the aerosol is when it is near the ground.
                        const double u = static_cast<double>(s + 1) / steps;
                        const double t1 = len * u * u;
                        const double dt = t1 - prev;
                        const V3 q = pos + dir * (0.5 * (prev + t1));
                        prev = t1;
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
// sunlight above the atmosphere: single scattering plus the multiple-scattering table. Steps
// finer near the eye, where the aerosol is.
V3 skyInScatter(const SkyParams& p, const CpuTables& c, double r, const V3& dir, const V3& sun,
                int steps)
{
    const bool ground = hitsGround(r, dir.y);
    const double len = ground ? raySphere(r, dir.y, kGroundRadius)
                              : std::max(raySphere(r, dir.y, kTopRadius), 0.0);
    const double cosTheta = dot(dir, sun);
    const double pr = rayleighPhase(cosTheta), pm = miePhase(p.g, cosTheta);
    const V3 pos = v3(0.0, r, 0.0);
    V3 thr = v3(1, 1, 1), l = v3(0, 0, 0);
    double prev = 0.0;
    for (int s = 0; s < steps; ++s)
    {
        const double u = static_cast<double>(s + 1) / steps;
        const double t1 = len * u * u;
        const double dt = t1 - prev;
        const V3 q = pos + dir * (0.5 * (prev + t1));
        prev = t1;
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
            out[y * w + x] = skyInScatter(p, c, r, dir, sun, 40) * scale;
        }
    }
}

// ---- cloud light (render milestone 12, task 1) ----
// The light a cloud receives at its altitude and position, from this model (after Gaijin's Dagor
// daSkies2, `clouds_light_color`; BSD-3-Clause): the clouds' include (RaCloud.fxh) reads it per
// sample. One 2D texture of kCloudLightLayers tiles across (altitudes above the level's ground,
// layer i at (i + 0.5) / layers of kCloudLightTopKm) by three rows of tiles down: the sun's
// irradiance there, the sky's mean radiance from above (its irradiance / pi), and the ground's
// radiance from below. A tile's texel (x, z) sits at world x, z = ((i + 0.5) / tile * 2 - 1) times
// kCloudLightRangeKm from the level's origin: the planet's curvature lowers the sun far away
// (sunset on the distant deck first). The ground everywhere has the level's albedo.
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

void buildCloudLight(const SkyParams& p, const CpuTables& c, const V3& toSun, const V3& sunTop,
                     std::vector<V3>& out)
{
    const int w = kCloudLightLayers * kCloudLightTile, h = kCloudLightRows * kCloudLightTile;
    out.assign(w * h, v3(0, 0, 0));
    const double rg = kGroundRadius + std::max(p.groundKm, 0.001);
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

double luminance(const V3& c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }

// ---- inputs ----

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

// ---- the builds (worker thread) ----

// A build: on a change of the weather or the authored light every table; on the camera's move
// to another height band only its sky view, from the live tables.
struct Job
{
    // In.
    bool weather;
    SkyParams air, ref;
    V3 toSun;              // world, unit
    V3 sunGround;          // the authored sun (the surfaces' at the reference air)
    double bandKm;         // the camera's sky view: its height above sea level
    V3 sunTop;             // band jobs: the live one
    CpuTables tables;      // band jobs: a copy of the live ones
    Inputs inputs;         // weather jobs: the authored light it was built for
    double seaOffset;      // weather jobs: m, world y 0's height above sea level
    // Out.
    V3 sunFactor;          // weather jobs: the surfaces' sun over the authored
    V3 zenith, horizon;    // weather jobs: the sky at the ground, for the tools
    std::vector<V3> groundSkyView, skyView, cloudLight;
    double ms;
};

void computeJob(Job& job)
{
    const LARGE_INTEGER zero = {};
    LARGE_INTEGER t0 = zero, t1 = zero, freq = zero;
    QueryPerformanceCounter(&t0);
    const double sinE = std::min(std::max(job.toSun.y, -1.0), 1.0);
    const V3 sunFrame = v3(std::sqrt(std::max(0.0, 1.0 - sinE * sinE)), sinE, 0.0);
    const double rGround = kGroundRadius + std::max(job.air.groundKm, 0.001);
    if (job.weather)
    {
        // The sun above the atmosphere against the reference air; the surfaces' through this air.
        const double muS = std::max(sinE, 0.02);
        const V3 tRef = vmax(transmittanceMarch(job.ref, rGround, muS, 64), 0.1);
        const V3 tAir = transmittanceMarch(job.air, rGround, muS, 64);
        job.sunTop = job.sunGround * kPi / tRef;
        job.sunFactor = tAir / tRef;
        buildTransmittance(job.air, job.tables, kTransmittanceW, kTransmittanceH);
        buildMultiScatter(job.air, job.tables, kMultiScatterSize);
        buildCloudLight(job.air, job.tables, job.toSun, job.sunTop, job.cloudLight);
        buildSkyView(job.air, job.tables, rGround, sunFrame, job.sunTop, kSkyViewW, kSkyViewH,
                     job.groundSkyView);
        const V3 sunMu = v3(std::sqrt(1.0 - muS * muS), muS, 0.0);
        job.zenith = skyInScatter(job.air, job.tables, rGround, v3(0, 1, 0), sunMu, 40) * job.sunTop;
        const double ce = std::cos(kPi / 180.0), se = std::sin(kPi / 180.0);
        V3 sum = v3(0, 0, 0);
        for (int i = 0; i < 8; ++i)
        {
            const double a = 2.0 * kPi * (i + 0.5) / 8;
            sum = sum + skyInScatter(job.air, job.tables, rGround, v3(ce * std::cos(a), se, ce * std::sin(a)),
                                     sunMu, 40);
        }
        job.horizon = sum * (1.0 / 8) * job.sunTop;
    }
    const double rBand = kGroundRadius + std::max(job.bandKm, 0.001);
    if (job.weather && std::fabs(rBand - rGround) < 1e-6)
        job.skyView = job.groundSkyView;
    else
        buildSkyView(job.air, job.tables, rBand, sunFrame, job.sunTop, kSkyViewW, kSkyViewH, job.skyView);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    job.ms = freq.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / freq.QuadPart : 0.0;
}

// What a weather build is made from, to tell when it changes.
struct WeatherKey
{
    AtmosphereWeather weather;
    double seaOffset;
    double refHeight;
    V3 albedo;
    float toSun[3], sun[3];
};

bool sameKey(const WeatherKey& a, const WeatherKey& b)
{
    return a.weather.visibility == b.weather.visibility && a.weather.aerosol == b.weather.aerosol
        && a.weather.mixing == b.weather.mixing && a.weather.altitude == b.weather.altitude
        && a.seaOffset == b.seaOffset && a.refHeight == b.refHeight
        && a.albedo.x == b.albedo.x && a.albedo.y == b.albedo.y && a.albedo.z == b.albedo.z
        && std::memcmp(a.toSun, b.toSun, sizeof(a.toSun)) == 0 && std::memcmp(a.sun, b.sun, sizeof(a.sun)) == 0;
}

struct State
{
    bool haveInputs;
    Inputs inputs;
    // Read once per terrain.
    const void* terrainRead;
    double refHeight;        // m: the terrain's mean height
    V3 groundAlbedo;         // linear: the colormaps' mean colour
    // Requests.
    bool haveKey;
    WeatherKey key;          // the last weather build requested
    double seaOffset;        // m: altitude above sea level = world y + this
    const char* altitudeSource;
    double requestedBandKm;  // the last camera band requested (-1 none)
    Job* job;                // running
    Job* pending;            // to start when it ends
    // Live: the last finished builds, what the textures and constants hold.
    bool fitted;
    SkyParams sky;
    Inputs liveInputs;
    double liveSeaOffset;
    V3 sunTop;               // irradiance above the atmosphere (pi * scene sun units)
    V3 sunFactor;            // the surfaces' sun over the authored
    V3 zenithModel, horizonModel, horizonTarget;
    double skyGain;          // horizon luminance: authored over the model's (not applied)
    double bandKm;           // the bound sky view's height above sea level
    CpuTables tables;
    double fitMs, bandMs;
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
    bool effectMissing;
    bool atlasLive;          // this frame's atlas is drawn and bound to s9
    float cutRange;          // rendererViewDistanceCutRange
    // The sky cube (milestone 11 task 7, milestone 12 task 7): the ground's sky view and the
    // clouded sky from the ground.
    std::vector<float> skyViewRgb;   // the sky view at the ground
    std::vector<float> cloudSky;     // rgba, decoded; empty: clear
    unsigned int cloudSkyW, cloudSkyH;
    bool cubeDirty;
};

State g = {};
std::atomic<bool> g_jobDone(false);
AtmosphereWeather g_weather = { static_cast<float>(kRefVisibility), kRefAerosol,
                                static_cast<float>(kRefMixing), kAltitudeAuto };

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

// The surfaces' sun from an authored channel: Ra_SunIrradiance in the shaders (gamma 2.2, the x2
// overbright decoded with it), before the weather's factor.
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

// World y 0's height above sea level (m): from the weather's altitude (the level's ground), else
// the far ground's bake, else 0.
double seaOffsetFor(const AtmosphereWeather& w, double refHeight, const char*& source)
{
    if (w.altitude > kAltitudeAuto * 0.5f)
    {
        source = "weather";
        return w.altitude - refHeight;
    }
    float offset = 0.0f;
    if (geom::farGroundBakeSeaOffset(offset))
    {
        source = "bake";
        return offset;
    }
    source = "none: world y 0 at sea level";
    return 0.0;
}

double cameraY()
{
    return rendererGetRenderView()->getViewMatrixPtr()->m[13];
}

// The camera's band: its height above sea level in kSkyBandKm steps.
double bandFor(double cameraAltitudeKm)
{
    return std::max(std::floor(cameraAltitudeKm / kSkyBandKm + 0.5) * kSkyBandKm, 0.0);
}

void startJob(Job* job)
{
    if (!job->weather)
    {
        // A band job: the live air and tables.
        job->air = g.sky;
        job->sunTop = g.sunTop;
        job->toSun = v3(g.liveInputs.toSun[0], g.liveInputs.toSun[1], g.liveInputs.toSun[2]);
        job->tables = g.tables;
    }
    g.job = job;
    g_jobDone = false;
    // Detached: the job is only read back through g_jobDone, and a game closing mid-build ends it.
    std::thread([job]() {
        computeJob(*job);
        g_jobDone = true;
    }).detach();
}

void request(Job* job)
{
    if (!g.job)
    {
        if (job->weather || g.fitted)
            startJob(job);
        else
            delete job;
        return;
    }
    if (g.pending && g.pending->weather && !job->weather)
    {
        g.pending->bandKm = job->bandKm;   // the weather build makes the band's sky view too
        delete job;
        return;
    }
    delete g.pending;   // superseded
    g.pending = job;
}

void requestWeather(const WeatherKey& key)
{
    Job* const job = new Job();
    job->weather = true;
    const double groundKm = (key.refHeight + key.seaOffset) / 1000.0;
    job->air = makeAir(key.weather.visibility, key.weather.aerosol, key.weather.mixing, groundKm, key.albedo);
    job->ref = makeAir(kRefVisibility, kRefAerosol, kRefMixing, groundKm, key.albedo);
    job->toSun = v3(key.toSun[0], key.toSun[1], key.toSun[2]);
    job->sunGround = v3(key.sun[0], key.sun[1], key.sun[2]);
    job->bandKm = bandFor((cameraY() + key.seaOffset) / 1000.0);
    job->inputs = g.inputs;
    job->seaOffset = key.seaOffset;
    g.requestedBandKm = job->bandKm;
    g.seaOffset = key.seaOffset;
    request(job);
}

void requestBand(double bandKm)
{
    Job* const job = new Job();
    job->weather = false;
    job->bandKm = bandKm;
    g.requestedBandKm = bandKm;
    request(job);
}

void releaseTexture(IDirect3DTexture9*& t);
bool uploadTable(IDirect3DTexture9*& texture, const std::vector<V3>& data, unsigned int w, unsigned int h);

// A finished build made live: its tables uploaded, its constants used from this frame.
void finishJob()
{
    if (!g.job || !g_jobDone)
        return;
    Job* const job = g.job;
    g.job = 0;
    bool ok = uploadTable(g.skyView, job->skyView, kSkyViewW, kSkyViewH);
    if (job->weather)
    {
        ok = ok && uploadTable(g.trans, job->tables.trans, kTransmittanceW, kTransmittanceH)
                && uploadTable(g.ms, job->tables.ms, kMultiScatterSize, kMultiScatterSize)
                && uploadTable(g.cloudLightTexture, job->cloudLight, kCloudLightLayers * kCloudLightTile,
                               kCloudLightRows * kCloudLightTile);
        g.sky = job->air;
        g.tables.tw = job->tables.tw;
        g.tables.th = job->tables.th;
        g.tables.msn = job->tables.msn;
        g.tables.trans.swap(job->tables.trans);
        g.tables.ms.swap(job->tables.ms);
        g.sunTop = job->sunTop;
        g.sunFactor = job->sunFactor;
        g.zenithModel = job->zenith;
        g.horizonModel = job->horizon;
        g.liveInputs = job->inputs;
        g.liveSeaOffset = job->seaOffset;
        g.fitMs = job->ms;
        const Inputs& in = g.liveInputs;
        g.horizonTarget = vmax(v3(srgbToLinear(in.fog[0] / 255.0), srgbToLinear(in.fog[1] / 255.0),
                                  srgbToLinear(in.fog[2] / 255.0)), 1e-5);
        g.skyGain = luminance(g.horizonTarget) / std::max(luminance(g.horizonModel), 1e-9);
        g.skyViewRgb.resize(job->groundSkyView.size() * 3);
        for (size_t i = 0; i < job->groundSkyView.size(); ++i)
        {
            g.skyViewRgb[i * 3 + 0] = static_cast<float>(job->groundSkyView[i].x);
            g.skyViewRgb[i * 3 + 1] = static_cast<float>(job->groundSkyView[i].y);
            g.skyViewRgb[i * 3 + 2] = static_cast<float>(job->groundSkyView[i].z);
        }
        g.cubeDirty = true;
        const AerosolType& type = kAerosols[std::min(std::max(g_weather.aerosol, 0), kAerosolCount - 1)];
        std::fprintf(stderr,
            "[ATMOS] built in %.0f ms: ground %.0f m above sea level (%s), sun elevation %.1f deg, fog end %.0f m\n"
            "[ATMOS]   aerosol %s, extinction %.4f %.4f %.4f /km, mixing layer to %.0f m\n"
            "[ATMOS]   sun ground %.3f %.3f %.3f -> above %.3f %.3f %.3f (irradiance); surfaces' factor %.3f %.3f %.3f\n"
            "[ATMOS]   zenith %.4f %.4f %.4f, horizon %.4f %.4f %.4f; ground albedo %.3f %.3f %.3f\n",
            job->ms, g.sky.groundKm * 1000.0, g.altitudeSource,
            std::asin(std::min(std::max(static_cast<double>(in.toSun[1]), -1.0), 1.0)) * 180.0 / kPi, in.fogEnd,
            type.name, g.sky.aerosolExtinction.x, g.sky.aerosolExtinction.y, g.sky.aerosolExtinction.z,
            g.sky.mixingTopKm * 1000.0,
            job->sunGround.x, job->sunGround.y, job->sunGround.z, g.sunTop.x, g.sunTop.y, g.sunTop.z,
            g.sunFactor.x, g.sunFactor.y, g.sunFactor.z,
            g.zenithModel.x, g.zenithModel.y, g.zenithModel.z, g.horizonModel.x, g.horizonModel.y,
            g.horizonModel.z, g.sky.groundAlbedo.x, g.sky.groundAlbedo.y, g.sky.groundAlbedo.z);
    }
    else
        g.bandMs = job->ms;
    g.bandKm = job->bandKm;
    g.fitted = g.fitted || (job->weather && ok);
    if (!ok)
        std::fprintf(stderr, "[ATMOS] tables not uploaded\n");
    delete job;
    if (g.pending)
    {
        Job* const next = g.pending;
        g.pending = 0;
        startJob(next);
    }
}

// The sky cube and its irradiance from the ground's sky view, with the clouds.
void updateSkyCube()
{
    if (!g.cubeDirty || g.skyViewRgb.empty())
        return;
    g.cubeDirty = false;
    SkyCubeSource src;
    src.skyView = &g.skyViewRgb[0];
    src.skyViewW = kSkyViewW;
    src.skyViewH = kSkyViewH;
    src.groundRadiusKm = kGroundRadius;
    src.altitudeKm = std::max(g.sky.groundKm, 0.001);
    for (int k = 0; k < 3; ++k)
    {
        src.toSun[k] = g.liveInputs.toSun[k];
        src.sun[k] = static_cast<float>(g.liveInputs.sun[k] * at(g.sunFactor, k));
        src.groundAlbedo[k] = static_cast<float>(at(g.sky.groundAlbedo, k));
    }
    // The air's extinction at the ground (1/m, 550 nm), for the ground seen below the horizon.
    const Media m = media(g.sky, g.sky.groundKm);
    src.groundExtinction = m.extinction.y / 1000.0;
    src.clouds = g.cloudSky.empty() ? 0 : &g.cloudSky[0];
    src.cloudsW = g.cloudSkyW;
    src.cloudsH = g.cloudSkyH;
    rendererSkyCubeBuild(src);
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

// RaAtmosphere's 16 float4s (contract 26; layout in RaAtmosphere.fx).
void fillConstants(float c[16][4])
{
    std::memset(c, 0, sizeof(float) * 64);
    const Inputs& in = g.liveInputs;
    const SkyParams& p = g.sky;
    const float rows[11][4] = {
        { float(p.rayleigh.x), float(p.rayleigh.y), float(p.rayleigh.z), float(kGroundRadius) },
        { float(p.aerosolScattering.x), float(p.aerosolScattering.y), float(p.aerosolScattering.z),
          float(kTopRadius) },
        { float(p.aerosolExtinction.x), float(p.aerosolExtinction.y), float(p.aerosolExtinction.z), float(p.g) },
        { float(kOzone[0]), float(kOzone[1]), float(kOzone[2]), float(std::max(g.bandKm, 0.001)) },
        { float(g.sunTop.x), float(g.sunTop.y), float(g.sunTop.z), 0.0f },
        { in.toSun[0], in.toSun[1], in.toSun[2], 0.0f },
        { float(p.groundAlbedo.x), float(p.groundAlbedo.y), float(p.groundAlbedo.z), 0.0f },
        { float(p.groundKm), float(p.mixingTopKm), float(1.0 / kAerosolHeight), float(g.liveSeaOffset) },
        { 0.0f, 0.0f, 0.0f, 1.0f },   // the air's light under the clouds, below
        { float(in.sun[0] * g.sunFactor.x), float(in.sun[1] * g.sunFactor.y), float(in.sun[2] * g.sunFactor.z),
          g.inputs.fogEnd },
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

bool drawPass(void* effect, const std::string& technique, IDirect3DTexture9* target, float c[16][4],
              const float cloudShadow[2][4])
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
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaTransmittanceTable"))
        d3dx->SetTexture(h, g.trans);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaMultiScatterTable"))
        d3dx->SetTexture(h, g.ms);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudShadow"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(cloudShadow), kCloudShadowVec4s);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudShadowMap"))
        d3dx->SetTexture(h, cloudShadow[0][3] > 0.0f ? rendererCloudShadowMap() : 0);

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

AtmosphereWeather& rendererAtmosphereWeather()
{
    return g_weather;
}

const char* rendererAtmosphereAerosolName(int type)
{
    return kAerosols[std::min(std::max(type, 0), kAerosolCount - 1)].name;
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
    struct BindSkyCube   // milestone 11 task 7: at every return below, while the model draws the sky
    {
        ~BindSkyCube() { rendererSkyCubeFrame(rendererAtmosphereDrawsSky()); }
    } bindSkyCube;
    finishJob();
    Inputs in;
    if (!gatherInputs(in))
        return;
    // Under water the fog constants are the water's: keep the last inputs.
    if ((!g.haveInputs || !sameInputs(in, g.inputs)) && !cameraIsUnderWater())
    {
        g.inputs = in;
        g.haveInputs = true;
        if (in.terrain != g.terrainRead || !g.fitted)
            readTerrain(in.terrain);
    }
    if (!g.haveInputs)
        return;
    WeatherKey key;
    std::memset(&key, 0, sizeof(key));
    key.weather = g_weather;
    key.refHeight = g.refHeight;
    key.seaOffset = seaOffsetFor(g_weather, g.refHeight, g.altitudeSource);
    key.albedo = g.groundAlbedo;
    std::memcpy(key.toSun, g.inputs.toSun, sizeof(key.toSun));
    std::memcpy(key.sun, g.inputs.sun, sizeof(key.sun));
    if (!g.haveKey || !sameKey(key, g.key))
    {
        g.key = key;
        g.haveKey = true;
        requestWeather(key);
    }
    else
    {
        // The camera's sky view: its band, moved once the camera is kSkyBandSlackKm from it.
        const double altitude = (cameraY() + g.seaOffset) / 1000.0;
        if (std::fabs(altitude - g.requestedBandKm) > kSkyBandSlackKm && g.fitted)
            requestBand(bandFor(altitude));
    }
    if (!g.fitted)
        return;
    updateSkyCube();
}

bool rendererAtmosphereSettling()
{
    return g.job || g.pending || g.cubeDirty;
}

void rendererAtmosphereDrawAtlas(const float cloudShadow[2][4])
{
    g.atlasLive = false;
    if (!device() || !rendererAtmosphereDrawsSky() || !ensureAtlas())
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
    g.atlasLive = drawPass(effect, kAp, g.atlas, c, cloudShadow);
    endPasses(saved);
    effectRelease(static_cast<Effect*>(effect));
    if (g.atlasLive)
        bindAtlas(g.atlas);
}

void rendererAtmosphereAerialConstants(float c[4])
{
    for (int i = 0; i < 4; ++i)
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
}

void rendererAtmosphereSunFactor(float c[4])
{
    const bool live = g.fitted;
    c[0] = live ? static_cast<float>(g.sunFactor.x) : 0.0f;
    c[1] = live ? static_cast<float>(g.sunFactor.y) : 0.0f;
    c[2] = live ? static_cast<float>(g.sunFactor.z) : 0.0f;
    c[3] = live ? 1.0f : 0.0f;
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
    i.bandMs = static_cast<float>(g.bandMs);
    i.refHeight = static_cast<float>(g.refHeight);
    i.fogEnd = g.inputs.fogEnd;
    i.skyGain = static_cast<float>(g.skyGain);
    i.weather = g_weather;
    i.aerosolName = rendererAtmosphereAerosolName(g_weather.aerosol);
    i.groundAltitude = static_cast<float>(g.sky.groundKm * 1000.0);
    i.mixingTop = static_cast<float>(g.sky.mixingTopKm * 1000.0);
    i.seaOffset = static_cast<float>(g.liveSeaOffset);
    i.altitudeSource = g.altitudeSource ? g.altitudeSource : "";
    i.skyViewAltitude = static_cast<float>(g.bandKm * 1000.0);
    i.aerosolG = static_cast<float>(g.sky.g);
    for (int k = 0; k < 3; ++k)
    {
        i.toSun[k] = g.liveInputs.toSun[k];
        i.sunTop[k] = static_cast<float>(at(g.sunTop, k));
        i.sunFactor[k] = static_cast<float>(at(g.sunFactor, k));
        i.aerosolExtinction[k] = static_cast<float>(at(g.sky.aerosolExtinction, k));
        i.aerosolScattering[k] = static_cast<float>(at(g.sky.aerosolScattering, k));
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
    return g.fitted && g.skyView;
}

// The model's sky (contract 7, SkyDome.fx technique RaSky): one full-screen pass at the far plane
// on the sky dome's effect, in the bound target and viewport, testing the scene depth without
// writing it. Returns false, drawing nothing, while the model does not draw the sky
// (no build yet).
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

    // The sun disc: the surfaces' sun (the authored sun through this weather's air) spread over the
    // disc's solid angle, so the disc's irradiance is the sun's; its luminance capped so the MSAA
    // resolve of its edge stays within a pixel (bloom, milestone 13, spreads it).
    const double radius = kSunAngularRadius * kPi / 180.0;
    const double solidAngle = 2.0 * kPi * (1.0 - std::cos(radius));
    V3 disc = v3(g.liveInputs.sun[0], g.liveInputs.sun[1], g.liveInputs.sun[2]) * g.sunFactor * (kPi / solidAngle);
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

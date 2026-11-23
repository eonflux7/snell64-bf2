// [RENDER] Sky clouds' baked volumes (render milestone 12, task 2,
// docs/lanes/render.md#12-clouds). Not part of retail BF2.
//
// The structure is Gaijin's Dagor daSkies2 (BSD-3-Clause, Copyright (c) 2023 Gaijin
// Entertainment): a weather map over a tiled 65 km world, a baked density field marched instead
// of noise per step, a coarse copy to skip empty space, and a volume of the light reaching each
// point. Dagor renders into volume slices (D3D10+); here every volume is a 2D atlas of its slices
// drawn by pixel passes of Shaders/RaClouds (shader contract 13), over several frames:
//
//   weather   128 x 128 A8R8G8B8, managed, built here on the CPU: r the low layer's coverage,
//             g the mid layer's, b the cloud type (0 stratus, 0.5 cumulus, 1 cumulonimbus)
//   shape     128 x 128 x 32 L8 volume, managed, built here once: Perlin-Worley (Schneider 2015)
//   detail    32 x 32 x 32 L8 volume, managed, built here once: Worley octaves, for the erosion at
//             sample time (task 3)
//   field     512 x 512 x 64 density, 2048 x 2048 A8R8G8B8: 4 x 4 tiles of 512 x 512, tile k holds
//             slices 4k .. 4k + 3 in r g b a. 128 m across, the slab's thickness / 64 up
//   coarse    64 x 64 x 8, each the field's maximum over 8 x 8 x 8, 128 x 64 A8R8G8B8 (2 tiles
//             of 4 slice groups)
//   light     256 x 256 x 64, 2048 x 2048 A16B16G16R16F: 8 x 8 tiles, one slice each: r the
//             transmittance toward the sun at the last scattering octave's extinction, g the
//             ambient's visibility from above (sky), b from below (ground)
//
// Task 3 (contract 14): the panorama, 2048 x 1024 A16B16G16R16F over the upper hemisphere from the
// level's origin (RaCloudPanorama.fxh), marched through the live set by technique RaCloudPanorama
// a band of 128 rows a frame. Three in a ring: the sky (rendererCloudsBindSky) blends the previous
// and the newest complete one by the progress of the one being drawn, so a new panorama fades in
// over the 8 frames the next one takes (Dagor's daSkiesPanorama). Each is blended into the newest
// complete one (weight kPanoHistoryWeight), so its march's jitter averages out across panoramas
// instead of showing as grain that changes with each. Each is drawn from the set and
// the time latched at its first band; the wind drifts the field under it. renderer.clouds 0 stops
// the builds and the panorama and draws the clear sky.
//
// Task 4 (contract 15): reachable clouds. The panorama becomes the far field: the whole sphere from
// the camera's position latched at its first band, marched from the handoff distance
// (renderer.cloudNear). Nearer, rendererCloudsNear marches the clouds per texel of a target half
// the view's size, from the camera to the handoff or the scene's depth, resolves it against the
// reprojected history (the camera's motion and the wind's drift) and composites it over the
// scene after the sky, before the world's transparents.
//
// Tasks 7-8 (contract 16): a Beer shadow map of the clouds over 32 km around the camera (technique
// RaCloudShadow, a band a frame, again about every second; the wind's drift since it was drawn is
// applied by the receivers' constants), and a small ground panorama from the level's origin, read
// back about every 30 s, from which the atmosphere rebuilds the sky cube, the sky light and the
// haze's light under the clouds.
//
// The weather evolves (each region's clouds grow and decay over 10-30 minutes); every
// kRebuildSeconds a new set is built into the other half of a double buffer, two field tiles and
// eight light tiles a frame, then made live. Presets (desert, maritime) set the slab, the cloud
// types, the water content and the droplets; renderer.cloudCoverage sets how many.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Mat4.h"
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
namespace dx9rend {

extern void* g_rendererBase;
extern void* g_shaderManager;
void* effectD3dxHandle(void* effect);   // Effect.cpp

namespace {

const double kPi = 3.14159265358979;

const unsigned int kWeatherSize = 128;
const unsigned int kShapeXz = 128, kShapeY = 32, kDetailSize = 32;
const unsigned int kFieldXz = 512, kFieldSlices = 64, kFieldTiles = 4;       // 4 x 4 tiles
const unsigned int kFieldAtlas = kFieldXz * kFieldTiles;                    // 2048
const unsigned int kCoarseXz = 64;                                           // 128 x 64: 2 tiles
const unsigned int kLightXz = 256, kLightTiles = 8;                          // 8 x 8 tiles
const unsigned int kLightAtlas = kLightXz * kLightTiles;                    // 2048
const double kWorldSize = 65536.0;   // m, the tiled weather world
const double kRebuildSeconds = 10.0;
const unsigned int kFieldTilesPerFrame = 2, kLightTilesPerFrame = 8;
const unsigned int kPanoW = 2048, kPanoH = 1024, kPanoBands = 8;             // RaCloudPanorama.fxh
const double kPlanetRadius = 6360000.0;   // m, the atmosphere's ground radius (Atmosphere.cpp)
const double kPanoReach = 150000.0;       // m: the march stops here (full haze near the horizon)
const double kDetailTileM = 1500.0;       // m: the detail noise's tile
const double kHighStreak = 10.0;          // the high cloud's streaks: length over width
const unsigned int kRows = 20;            // RaClouds' float4s (RaCloudField.fxh, RaClouds.fx)
const float kNearHistoryWeight = 0.1f;    // the current frame's weight in the near clouds' history
const float kPanoHistoryWeight = 0.25f;   // a new panorama's weight against the previous one
const unsigned int kShadowSize = 1024, kShadowBands = 8;   // RaClouds.fx RA_CLOUD_SHADOW_SIZE
const double kShadowWorld = 32768.0;      // m: the shadow map's side (32 m texels)
const double kShadowSeconds = 1.0;        // a new shadow map starts this long after the last
const double kSunRiseMin = 0.035;         // the sun's lowest y for the shadow map (RaClouds.fx)
const unsigned int kSkyLightW = 256, kSkyLightH = 128;   // the ground panorama for the sky light
const double kSkyLightSeconds = 30.0;     // its refresh
const unsigned int kSkyLightReadDelay = 2;   // frames between its draw and its readback

struct Preset
{
    const char* name;
    double baseM, topM;            // the low layer above the reference height (m)
    double midBaseM, midTopM;      // the mid layer's (m); the slab spans both (Dagor's CloudsForm layers)
    double midDensity;             // the mid layer's density against the low one's
    double midCoverage;            // the mid layer's coverage against renderer.cloudCoverage
    double typeLo, typeHi;         // the weather's cloud type range
    double lwc;                    // g/m^3 at density 1
    double dropletUm;              // droplet diameter
    double windX, windZ, windMs;   // the wind at cloud level: direction (world x, z), speed
    double highM, highCoverage, highTau;   // the high cloud: height, coverage, vertical optical depth
};
// Desert: a hot dry afternoon (T 35 C, dew point 19 C: base ~125 m x 16 = 2 km; owner, 2026-11-22:
// 3.5 km sat too high for 4 km maps), cumulus from small to towering up to 6 km (a wide type
// range), under a patchy altocumulus sheet at 6.5-8 km; continental air, many small droplets.
// Maritime: a moist boundary layer (T - Td ~ 4 C: base ~500 m) under an inversion, stratus to
// stratocumulus up to 4.5 km, a thin mid layer at 3.8-5.5 km; few, large droplets. Both carry
// thin cirrus above the slab (vertical optical depth 0.3-0.4 where densest, the
// common range of 0.03-1), streaked along the wind.
const Preset kPresets[2] = {
    { "desert", 2000.0, 6000.0, 6500.0, 8000.0, 0.6, 0.8, 0.2, 0.6, 0.6, 12.0, 0.94, 0.34, 8.0, 11000.0, 0.35, 0.3 },
    { "maritime", 500.0, 4500.0, 3800.0, 5500.0, 0.35, 0.6, 0.0, 0.3, 0.35, 20.0, 0.6, -0.8, 12.0, 8000.0, 0.5, 0.4 },
};

// ---- noise (tileable) ----

inline unsigned int hash3(int x, int y, int z, unsigned int seed)
{
    unsigned int h = static_cast<unsigned int>(x) * 0x8da6b343u ^ static_cast<unsigned int>(y) * 0xd8163841u
                   ^ static_cast<unsigned int>(z) * 0xcb1ab31fu ^ seed * 0x9e3779b9u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
inline double unitHash(unsigned int h) { return (h & 0xffffff) * (1.0 / 16777216.0); }
inline int wrap(int v, int period) { const int m = v % period; return m < 0 ? m + period : m; }
inline double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

// Gradient noise in about [-1, 1], period `period` lattice cells along each axis.
double perlin3(double x, double y, double z, int period, unsigned int seed)
{
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y)),
              zi = static_cast<int>(std::floor(z));
    const double fx = x - xi, fy = y - yi, fz = z - zi;
    double n[8];
    for (int c = 0; c < 8; ++c)
    {
        const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
        const unsigned int h = hash3(wrap(xi + dx, period), wrap(yi + dy, period), wrap(zi + dz, period), seed);
        // 12 edge gradients.
        static const double g[12][3] = { { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 },
                                         { 1, 0, 1 }, { -1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 },
                                         { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 } };
        const double* gr = g[h % 12];
        n[c] = gr[0] * (fx - dx) + gr[1] * (fy - dy) + gr[2] * (fz - dz);
    }
    const double u = fade(fx), v = fade(fy), w = fade(fz);
    const double x00 = n[0] + (n[1] - n[0]) * u, x10 = n[2] + (n[3] - n[2]) * u;
    const double x01 = n[4] + (n[5] - n[4]) * u, x11 = n[6] + (n[7] - n[6]) * u;
    const double y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    return (y0 + (y1 - y0) * w);
}

// Octaves of perlin3 over the unit cube, `cells` cells across at the first. Not normalized, as
// Dagor's get_perlin_N_octaves: the amplitudes sum to nearly 2, and its thresholds expect that.
double perlinFbm(double x, double y, double z, int cells, int octaves, unsigned int seed)
{
    double sum = 0.0, amp = 1.0;
    for (int o = 0; o < octaves; ++o)
    {
        const int c = cells << o;
        sum += amp * perlin3(x * c, y * c, z * c, c, seed + o);
        amp *= 0.5;
    }
    return sum;
}

// Worley F1, squared, `cells` cells across the unit cube (Dagor's voronoi().r).
double worley(double x, double y, double z, int cells, unsigned int seed)
{
    const double px = x * cells, py = y * cells, pz = z * cells;
    const int xi = static_cast<int>(std::floor(px)), yi = static_cast<int>(std::floor(py)),
              zi = static_cast<int>(std::floor(pz));
    double best = 1e9;
    for (int k = -1; k <= 1; ++k)
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const unsigned int h = hash3(wrap(xi + i, cells), wrap(yi + j, cells), wrap(zi + k, cells), seed);
                const double fx = xi + i + unitHash(h) - px;
                const double fy = yi + j + unitHash(h * 0x2545f491u + 1u) - py;
                const double fz = zi + k + unitHash(h * 0x9e3779b1u + 7u) - pz;
                best = std::min(best, fx * fx + fy * fy + fz * fz);
            }
    return std::min(best, 1.0);
}

// Dagor's get_worley_3_octaves2: 1 - (w(s) + w(2s) / 2 + w(4s) / 4).
double worley3(double x, double y, double z, int cells, unsigned int seed)
{
    return 1.0 - (worley(x, y, z, cells, seed) + 0.5 * worley(x, y, z, cells * 2, seed + 1)
                  + 0.25 * worley(x, y, z, cells * 4, seed + 2));
}

double clamp01(double v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
double remap(double v, double a, double b, double c, double d) { return c + (v - a) / (b - a) * (d - c); }

// Dagor's get_cloud_shape (generate_clouds2.dshl): Perlin dilated by Worley, eroded by three
// Worley octaves; dilation 0.3, erosion 1, gamma 1.25. Five Perlin octaves, not seven: the
// texture holds 128 texels across, and the sixth and seventh (128 and 256 cells) only alias.
double cloudShape(double x, double y, double z)
{
    const double perlin = clamp01(remap(perlinFbm(x, y, z, 4, 5, 11u), -0.9, 0.9, 0.0, 1.0));
    const double wr = worley3(x, y, z, 7, 21u);
    const double wg = worley3(x, y, z, 6, 31u), wb = worley3(x, y, z, 11, 41u), wa = worley3(x, y, z, 23, 51u);
    const double perlinWorley = remap(perlin, (1.0 - wr) * 0.3, 1.0, 0.0, 1.0);
    const double shape = clamp01(remap(perlinWorley, -(1.0 - (0.625 * wg + 0.25 * wb + 0.124 * wa)), 1.0, 0.0, 1.0));
    return std::pow(shape, 1.25);
}

// Dagor's get_cloud_detail: three Worley octaves at 2, 7 and 11 cells.
double cloudDetail(double x, double y, double z)
{
    return clamp01(0.425 * worley3(x, y, z, 2, 61u) + 0.35 * worley3(x, y, z, 7, 71u)
                   + 0.35 * worley3(x, y, z, 11, 81u));
}

// One byte per texel, texel centres over the unit cube; rows split across threads.
void buildVolume(std::vector<unsigned char>& out, unsigned int w, unsigned int h, unsigned int d,
                 double (*fn)(double, double, double))
{
    out.assign(static_cast<size_t>(w) * h * d, 0);
    unsigned int threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned int t = 0; t < threads; ++t)
        pool.push_back(std::thread([&out, w, h, d, fn, t, threads]() {
            for (unsigned int z = t; z < d; z += threads)
                for (unsigned int y = 0; y < h; ++y)
                    for (unsigned int x = 0; x < w; ++x)
                    {
                        const double v = fn((x + 0.5) / w, (y + 0.5) / h, (z + 0.5) / d);
                        out[(static_cast<size_t>(z) * h + y) * w + x] =
                            static_cast<unsigned char>(clamp01(v) * 255.0 + 0.5);
                    }
        }));
    for (size_t i = 0; i < pool.size(); ++i)
        pool[i].join();
}

// ---- the weather (Dagor's gen_weather.dshl, on the CPU) ----

double setRange(double v, double lo, double hi) { return clamp01((v - lo) / (hi - lo)); }
double setRangeSmoother(double v, double lo, double hi, double pt)
{
    if (v > hi * pt)
        return setRange(hi * pt, lo, hi) + (1.0 - setRange(hi * pt, lo, hi)) * (v - hi * pt) / (1.0 - pt);
    return setRange(v, lo, hi);
}

struct WeatherInputs
{
    int preset;
    double coverage;
    // The layers above the reference height (m), the mid one's density and coverage: the preset's
    // or the console's. The slab spans both.
    double baseM, topM, midBaseM, midTopM, midDensity, midCoverage;
    double slabBase() const { return std::min(baseM, midBaseM); }
    double slabTop() const { return std::max(topM, midTopM); }
    double seconds;     // since the level started: the clouds' life phase
};

// Dagor's coverage to (humidity, gamma) for a layer's signal.
void coverageCurve(double coverage, double gammaRate, double& humidity, double& gamma)
{
    humidity = std::sqrt(2.0 * std::min(coverage, 0.5)) - 1.0;
    gamma = 1.5 * std::exp((2.0 * std::max(coverage, 0.5) - 1.0) * -gammaRate);
}

void buildWeather(const WeatherInputs& in, std::vector<unsigned int>& out)
{
    const Preset& p = kPresets[in.preset];
    out.assign(kWeatherSize * kWeatherSize, 0);
    double h1, g1, h2, g2;
    coverageCurve(in.coverage, 2.5, h1, g1);
    coverageCurve(in.midCoverage, 2.0, h2, g2);
    for (unsigned int y = 0; y < kWeatherSize; ++y)
        for (unsigned int x = 0; x < kWeatherSize; ++x)
        {
            const double u = (x + 0.5) / kWeatherSize, v = (y + 0.5) / kWeatherSize;
            // Life: each ~2 km region's signal swells and shrinks around its mean, its own phase and
            // period (10-30 min), so the mean coverage holds while the clouds evolve.
            const double phase = perlinFbm(u, v, 0.3, 32, 2, 91u) * 2.0;
            const double period = 600.0 + 1200.0 * clamp01(perlinFbm(u, v, 0.7, 16, 1, 93u) * 0.5 + 0.5);
            const double life = 0.18 * std::sin(2.0 * kPi * (in.seconds / period + phase));

            const double s1 = perlinFbm(u, v, 0.1, 2, 5, 101u) + life;
            const double l1 = std::pow(clamp01(setRangeSmoother(s1, -0.65, 0.65, 0.9) + h1), g1);
            const double s2 = perlinFbm(1.0 - v, 1.0 - u, 0.5, 4, 5, 111u) + 0.5 * life;
            const double l2 = std::pow(clamp01(setRangeSmoother(s2, -0.65, 0.65, 0.9) + h2), g2);

            const double typeSignal = clamp01(setRange(perlinFbm(u, v, 0.9, 3, 5, 121u), -0.25, 0.25));
            const double s5 = perlinFbm(u, v, 0.2, 9, 3, 131u), s6 = perlinFbm(1.0 - v, 1.0 - u, 0.4, 9, 3, 141u);
            const double k = 1.0 - (1.0 - typeSignal) * (1.0 - typeSignal);
            const double m5 = std::sqrt(std::max(s5 * 0.5 + 0.75, 0.0)) * (1.0 - k) + k;
            const double m6 = std::sqrt(std::max(s6 * 0.5 + 0.75, 0.0)) * (1.0 - k) + k;
            const double type = p.typeLo + (p.typeHi - p.typeLo) * typeSignal;

            const unsigned int r = static_cast<unsigned int>(clamp01(l1 * m5) * 255.0 + 0.5);
            const unsigned int g = static_cast<unsigned int>(clamp01(l2 * m6) * 255.0 + 0.5);
            const unsigned int b = static_cast<unsigned int>(clamp01(type) * 255.0 + 0.5);
            out[y * kWeatherSize + x] = (255u << 24) | (r << 16) | (g << 8) | b;
        }
}

// ---- state ----

enum Stage { kIdle, kWeather, kField, kCoarse, kLight };

struct VolumeSet
{
    IDirect3DTexture9* field;
    IDirect3DTexture9* coarse;
    IDirect3DTexture9* light;
};

struct State
{
    // Settings (console).
    float coverage;
    int preset;
    float baseM, topM;                // renderer.cloudBase, cloudTop: 0 the preset's
    float midBaseM, midTopM;          // renderer.cloudMidBase, cloudMidTop: 0 the preset's
    float midDensity, midCoverage;    // renderer.cloudMidDensity, cloudMidCoverage: < 0 the preset's
    bool enabled;
    // CPU-built.
    bool noiseStarted, noiseReady;
    std::atomic<bool> noiseDone;
    std::thread noiseThread;
    std::vector<unsigned char> shapeData, detailData;
    double noiseMs;
    IDirect3DVolumeTexture9* shape;
    IDirect3DVolumeTexture9* detail;
    IDirect3DTexture9* weather;
    double weatherMs;
    std::thread weatherThread;   // builds weatherData off the frame
    std::atomic<bool> weatherDone;
    std::vector<unsigned int> weatherData;
    WeatherInputs built;      // the weather of the set being built
    WeatherInputs liveInputs; // of the live set
    // GPU.
    VolumeSet sets[2];
    int live;                 // -1 none
    Stage stage;
    unsigned int next;        // the next tile of the stage
    unsigned int buildFrames;
    unsigned int lastBuildFrames;
    LARGE_INTEGER start, lastBuild;
    bool started;
    bool settingsChanged;
    bool changeBuild;         // the build under way was started by a settings change
    bool effectMissing;
    unsigned int builds;
    // The panorama (task 3).
    IDirect3DTexture9* pano[3];
    int panoNew, panoOld, panoDraw;   // ring indices; -1 none
    unsigned int panoBand;            // the next band of panoDraw; 0: the next one starts
    unsigned int panoramas;           // completed
    int panoSet;                      // the volume set the one being drawn reads
    WeatherInputs panoInputs;
    double panoSeconds;
    float panoObserver[3];            // world: the camera when the one being drawn started
    // The near clouds (task 4).
    float nearDistance;               // m, renderer.cloudNear; 0: none, the panorama from the camera
    IDirect3DTexture9* nearCurrent;   // this frame's march: light, transmittance
    IDirect3DTexture9* nearDepth;     // r the reprojection distance, g the scene depth used
    IDirect3DTexture9* nearHistory[2];
    unsigned int nearW, nearH, nearFrames;
    int nearLast;                     // the history resolved last frame; -1 none (invalid)
    float nearPrevRows[3][4];         // the previous frame's view (RaClouds[15..17])
    float nearPrevDrift[2];
    // The cloud shadow map (task 8).
    IDirect3DTexture9* shadow[2];
    int shadowLive, shadowDraw, shadowSet;   // -1 none
    unsigned int shadowBand, shadowMaps;
    WeatherInputs shadowInputs, liveShadowInputs;
    double shadowSeconds, liveShadowSeconds; // the wind's time each was drawn at
    float shadowCorner[2], liveShadowCorner[2];
    LARGE_INTEGER shadowStarted;
    // The sky light's ground panorama (task 7).
    IDirect3DTexture9* skyPano;
    IDirect3DSurface9* skySys;        // system memory, for the readback
    unsigned int skyStage;            // 0 idle; else frames since it was drawn, plus one
    bool skyDue, skyHanded;
    LARGE_INTEGER skyLast;
    unsigned int skyLights;
    std::vector<float> skyData;
};

State* g_state = 0;
State& st()
{
    if (!g_state)
    {
        g_state = new State();
        g_state->coverage = 0.4f;
        g_state->preset = 0;
        g_state->midDensity = g_state->midCoverage = -1.0f;
        g_state->enabled = true;
        g_state->live = -1;
        g_state->stage = kIdle;
        g_state->panoNew = g_state->panoOld = g_state->panoDraw = g_state->panoSet = -1;
        g_state->nearDistance = 16000.0f;
        g_state->nearLast = -1;
        g_state->shadowLive = g_state->shadowDraw = g_state->shadowSet = -1;
        g_state->skyDue = true;
    }
    return *g_state;
}

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

double secondsSince(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? static_cast<double>(t.QuadPart - t0.QuadPart) / f.QuadPart : 0.0;
}

void release(IDirect3DTexture9*& t)
{
    if (t)
        t->Release();
    t = 0;
}

void releaseSets()
{
    State& s = st();
    for (int i = 0; i < 2; ++i)
    {
        release(s.sets[i].field);
        release(s.sets[i].coarse);
        release(s.sets[i].light);
    }
    s.live = -1;
    if (s.stage != kWeather)   // a weather thread finishes into a new set
        s.stage = kIdle;
    for (int i = 0; i < 3; ++i)
        release(s.pano[i]);
    s.panoNew = s.panoOld = s.panoDraw = s.panoSet = -1;
    s.panoBand = 0;
    release(s.nearCurrent);
    release(s.nearDepth);
    release(s.nearHistory[0]);
    release(s.nearHistory[1]);
    s.nearW = s.nearH = 0;
    s.nearLast = -1;
    release(s.shadow[0]);
    release(s.shadow[1]);
    s.shadowLive = s.shadowDraw = s.shadowSet = -1;
    s.shadowBand = 0;
    release(s.skyPano);   // the system-memory copy survives a reset
    s.skyStage = 0;
    s.skyDue = true;
}

bool createTarget(unsigned int w, unsigned int h, D3DFORMAT format, IDirect3DTexture9*& t)
{
    if (t)
        return true;
    const HRESULT hr = device()->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, 0);
    if (hr < 0)
    {
        std::fprintf(stderr, "[CLOUDS] target %ux%u format %d failed: hr=0x%08lX\n", w, h,
                     static_cast<int>(format), static_cast<unsigned long>(hr));
        t = 0;
        return false;
    }
    return true;
}

bool ensureSet(VolumeSet& v)
{
    return createTarget(kFieldAtlas, kFieldAtlas, D3DFMT_A8R8G8B8, v.field)
        && createTarget(kCoarseXz * 2, kCoarseXz, D3DFMT_A8R8G8B8, v.coarse)
        && createTarget(kLightAtlas, kLightAtlas, D3DFMT_A16B16G16R16F, v.light);
}

bool uploadVolume(IDirect3DVolumeTexture9*& t, const std::vector<unsigned char>& data, unsigned int w,
                  unsigned int h, unsigned int d)
{
    if (!t && device()->CreateVolumeTexture(w, h, d, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &t, 0) < 0)
    {
        std::fprintf(stderr, "[CLOUDS] volume %ux%ux%u not created\n", w, h, d);
        t = 0;
        return false;
    }
    D3DLOCKED_BOX box;
    if (t->LockBox(0, &box, 0, 0) < 0)
        return false;
    for (unsigned int z = 0; z < d; ++z)
        for (unsigned int y = 0; y < h; ++y)
            std::memcpy(static_cast<unsigned char*>(box.pBits) + z * box.SlicePitch + y * box.RowPitch,
                        &data[(static_cast<size_t>(z) * h + y) * w], w);
    t->UnlockBox(0);
    return true;
}

bool uploadWeather(const std::vector<unsigned int>& data)
{
    State& s = st();
    if (!s.weather && device()->CreateTexture(kWeatherSize, kWeatherSize, 1, 0, D3DFMT_A8R8G8B8,
                                              D3DPOOL_MANAGED, &s.weather, 0) < 0)
    {
        s.weather = 0;
        return false;
    }
    D3DLOCKED_RECT lr;
    if (s.weather->LockRect(0, &lr, 0, 0) < 0)
        return false;
    for (unsigned int y = 0; y < kWeatherSize; ++y)
        std::memcpy(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, &data[y * kWeatherSize], kWeatherSize * 4);
    s.weather->UnlockRect(0);
    return true;
}

// RaClouds' constants (layout in RaClouds.fx).
void fillConstants(float c[kRows][4], const WeatherInputs& w, unsigned int first)
{
    std::memset(c, 0, sizeof(float) * 4 * kRows);
    const AtmosphereInfo a = rendererAtmosphereInfo();
    const Preset& p = kPresets[w.preset];
    const float base = static_cast<float>(a.refHeight + w.slabBase()), top = static_cast<float>(a.refHeight + w.slabTop());
    c[0][0] = static_cast<float>(-kWorldSize * 0.5);
    c[0][1] = static_cast<float>(-kWorldSize * 0.5);
    c[0][2] = static_cast<float>(1.0 / kWorldSize);
    c[0][3] = static_cast<float>(kWorldSize);
    c[1][0] = base;
    c[1][1] = top;
    c[1][2] = 1.0f / (top - base);
    c[1][3] = top - base;
    for (int k = 0; k < 3; ++k)
        c[2][k] = a.toSun[k];
    c[2][3] = static_cast<float>(p.lwc);
    c[3][0] = static_cast<float>(p.dropletUm);
    c[3][1] = static_cast<float>(w.midDensity);
    c[3][2] = 8.0f;    // shape tiles across the world (8 km each, Dagor's clouds_shape_scale)
    c[3][3] = 16.0f;   // the cumulonimbus shape's (4 km)
    c[4][0] = static_cast<float>(first);
    // Each layer's height fraction from the slab's: hf * x + y (Dagor's clouds_height_fractions).
    const double slab = w.slabTop() - w.slabBase();
    c[19][0] = static_cast<float>(slab / (w.topM - w.baseM));
    c[19][1] = static_cast<float>((w.slabBase() - w.baseM) / (w.topM - w.baseM));
    c[19][2] = static_cast<float>(slab / (w.midTopM - w.midBaseM));
    c[19][3] = static_cast<float>((w.slabBase() - w.midBaseM) / (w.midTopM - w.midBaseM));
}

bool drawTiles(void* effect, const char* technique, IDirect3DTexture9* target, unsigned int tileSize,
               unsigned int tilesAcross, unsigned int first, unsigned int count, float c[kRows][4],
               IDirect3DTexture9* field, IDirect3DTexture9* coarse)
{
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    if (!d3dx || !effectSetTechniqueByName(effect, technique))
        return false;
    State& s = st();
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaClouds"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudWeather"))
        d3dx->SetTexture(h, s.weather);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudShape"))
        d3dx->SetTexture(h, s.shape);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudField"))
        d3dx->SetTexture(h, field);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudCoarse"))
        d3dx->SetTexture(h, coarse);

    IDirect3DSurface9* surface = 0;
    target->GetSurfaceLevel(0, &surface);
    stateManagerSetRenderTarget(0, surface);
    flushRenderTargetsSeam();
    effectBegin(effect);
    effectBeginPass(effect, 0);
    for (unsigned int t = first; t < first + count; ++t)
    {
        const D3DVIEWPORT9 viewport = { (t % tilesAcross) * tileSize, (t / tilesAcross) * tileSize,
                                        tileSize, tileSize, 0.0f, 1.0f };
        device()->SetViewport(&viewport);
        geom::fsQuadDrawerDrawCurrentEffect();
    }
    effectEnd(effect);
    if (surface)
        surface->Release();
    return true;
}

void startNoise()
{
    State& s = st();
    s.noiseStarted = true;
    s.noiseThread = std::thread([]() {
        State& t = st();
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        buildVolume(t.shapeData, kShapeXz, kShapeXz, kShapeY, cloudShape);
        buildVolume(t.detailData, kDetailSize, kDetailSize, kDetailSize, cloudDetail);
        t.noiseMs = secondsSince(t0) * 1000.0;
        t.noiseDone = true;
    });
}

// One step of the build: the next stage's tiles into the set that is not live.
void buildStep(void* effect)
{
    State& s = st();
    const int target = s.live == 0 ? 1 : 0;
    if ((s.panoBand != 0 && s.panoSet == target) || (s.shadowBand != 0 && s.shadowSet == target))
        return;   // the panorama or the shadow map being drawn still reads it
    VolumeSet& v = s.sets[target];
    float c[kRows][4];
    fillConstants(c, s.built, s.next);
    ++s.buildFrames;
    if (s.stage == kField)
    {
        const unsigned int n = std::min(kFieldTilesPerFrame, kFieldTiles * kFieldTiles - s.next);
        drawTiles(effect, "RaCloudBuildField", v.field, kFieldXz, kFieldTiles, s.next, n, c, 0, 0);
        s.next += n;
        if (s.next >= kFieldTiles * kFieldTiles)
        {
            s.stage = kCoarse;
            s.next = 0;
        }
    }
    else if (s.stage == kCoarse)
    {
        drawTiles(effect, "RaCloudBuildCoarse", v.coarse, kCoarseXz, 2, 0, 2, c, v.field, 0);
        s.stage = kLight;
        s.next = 0;
    }
    else if (s.stage == kLight)
    {
        const unsigned int n = std::min(kLightTilesPerFrame, kLightTiles * kLightTiles - s.next);
        drawTiles(effect, "RaCloudBuildLight", v.light, kLightXz, kLightTiles, s.next, n, c, v.field, v.coarse);
        s.next += n;
        if (s.next >= kLightTiles * kLightTiles)
        {
            s.stage = kIdle;
            s.live = target;
            if (s.builds == 0 || s.built.preset != s.liveInputs.preset || s.built.coverage != s.liveInputs.coverage
                || s.built.baseM != s.liveInputs.baseM || s.built.topM != s.liveInputs.topM
                || s.built.midBaseM != s.liveInputs.midBaseM || s.built.midTopM != s.liveInputs.midTopM
                || s.built.midDensity != s.liveInputs.midDensity || s.built.midCoverage != s.liveInputs.midCoverage)
                s.skyDue = true;   // new settings: the sky light now, not at its next refresh
            s.liveInputs = s.built;
            s.lastBuildFrames = s.buildFrames;
            ++s.builds;
            if (s.builds == 1)
                std::fprintf(stderr, "[CLOUDS] first set live: %s, coverage %.2f, %u frames; noise %.0f ms, "
                             "weather %.1f ms\n", kPresets[s.built.preset].name, s.built.coverage,
                             s.buildFrames, s.noiseMs, s.weatherMs);
        }
    }
}

// RaClouds [5..8] for the panorama and the near clouds (layout in RaClouds.fx), observed from
// `observer` (world) with the near clouds out to `handoff` metres.
void fillPanoramaConstants(float c[kRows][4], const WeatherInputs& w, double seconds, const float observer[3],
                           float handoff)
{
    const AtmosphereInfo a = rendererAtmosphereInfo();
    const Preset& p = kPresets[w.preset];
    const double len = std::sqrt(p.windX * p.windX + p.windZ * p.windZ);
    const double dx = p.windX / len, dz = p.windZ / len;
    c[5][0] = static_cast<float>(std::fmod(dx * p.windMs * seconds, kWorldSize));
    c[5][1] = static_cast<float>(std::fmod(dz * p.windMs * seconds, kWorldSize));
    c[5][2] = observer[1];
    c[5][3] = static_cast<float>(kPlanetRadius);
    c[6][0] = static_cast<float>(a.refHeight + p.highM);
    c[6][1] = static_cast<float>(p.highCoverage);
    c[6][2] = static_cast<float>(p.highTau);
    c[6][3] = static_cast<float>(kHighStreak);
    c[7][0] = static_cast<float>(dx);
    c[7][1] = static_cast<float>(dz);
    c[7][2] = static_cast<float>(kDetailTileM);
    c[7][3] = static_cast<float>(kPanoReach);
    c[8][0] = observer[0];
    c[8][1] = observer[2];
    c[8][2] = a.refHeight;
    c[8][3] = handoff;
}

// The handoff distance this frame: the near clouds' reach, 0 while they are off.
float nearHandoff()
{
    const State& s = st();
    return s.nearDistance > 0.0f ? s.nearDistance : 0.0f;
}

// The main view's camera position (world).
void cameraPosition(float p[3])
{
    const float* const m = rendererGetRenderView()->getViewMatrixPtr()->m;
    p[0] = m[12];
    p[1] = m[13];
    p[2] = m[14];
}

// Binds the effect's cloud volumes and tables for a march (the panorama, the shadow map).
void bindMarchTextures(ID3DXEffect* d3dx, const VolumeSet& v, IDirect3DTexture9* history)
{
    State& s = st();
    const AtmosphereInfo a = rendererAtmosphereInfo();
    struct { const char* semantic; IDirect3DBaseTexture9* texture; } const textures[] = {
        { "RaCloudShape", s.shape }, { "RaCloudDetail", s.detail }, { "RaCloudField", v.field },
        { "RaCloudCoarse", v.coarse }, { "RaCloudLight", v.light }, { "RaCloudLightTable", a.cloudLight },
        { "RaSkyViewTable", a.skyView }, { "RaCloudPanoramaHistory", history },
    };
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i)
        if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, textures[i].semantic))
            d3dx->SetTexture(h, textures[i].texture);
}

// Binds the air's integral for the clouds' passes (RaAir.fxh, contract 32): the atmosphere's
// transmittance and multiple-scattering tables and the cloud shadow map with its constants, as the
// aerial-perspective atlas takes them, so far and near clouds are hazed as the ground is.
void bindAirTextures(ID3DXEffect* d3dx)
{
    const AtmosphereInfo a = rendererAtmosphereInfo();
    float shadow[2][4];
    rendererCloudShadowConstants(shadow);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaTransmittanceTable"))
        d3dx->SetTexture(h, a.transmittance);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaMultiScatterTable"))
        d3dx->SetTexture(h, a.multiScatter);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudShadow"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(shadow), kCloudShadowVec4s);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaCloudShadowMap"))
        d3dx->SetTexture(h, shadow[0][3] > 0.0f ? rendererCloudShadowMap() : 0);
}

// One full-screen pass of the current technique into `target`'s rows [y, y + rows).
void drawRows(void* effect, IDirect3DTexture9* target, unsigned int w, unsigned int y, unsigned int rows)
{
    IDirect3DSurface9* surface = 0;
    target->GetSurfaceLevel(0, &surface);
    stateManagerSetRenderTarget(0, surface);
    flushRenderTargetsSeam();
    const D3DVIEWPORT9 viewport = { 0, y, w, rows, 0.0f, 1.0f };
    device()->SetViewport(&viewport);
    effectBegin(effect);
    effectBeginPass(effect, 0);
    geom::fsQuadDrawerDrawCurrentEffect();
    effectEnd(effect);
    if (surface)
        surface->Release();
}

// The wind's unit direction for a preset.
void windDirection(const Preset& p, double& dx, double& dz)
{
    const double len = std::sqrt(p.windX * p.windX + p.windZ * p.windZ);
    dx = p.windX / len;
    dz = p.windZ / len;
}

// One band of the panorama being drawn; a finished one joins the ring as the newest.
void panoramaStep(void* effect)
{
    State& s = st();
    for (int i = 0; i < 3; ++i)
        if (!createTarget(kPanoW, kPanoH, D3DFMT_A16B16G16R16F, s.pano[i]))
            return;
    if (s.panoBand == 0)
    {
        s.panoSet = s.live;
        s.panoInputs = s.liveInputs;
        s.panoSeconds = secondsSince(s.start);
        cameraPosition(s.panoObserver);
        for (int i = 0; i < 3; ++i)
            if (i != s.panoNew && i != s.panoOld)
            {
                s.panoDraw = i;
                break;
            }
    }
    const VolumeSet& v = s.sets[s.panoSet];
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    if (!d3dx || !v.field || !effectSetTechniqueByName(effect, "RaCloudPanorama"))
        return;
    float c[kRows][4];
    fillConstants(c, s.panoInputs, 0);
    fillPanoramaConstants(c, s.panoInputs, s.panoSeconds, s.panoObserver, nearHandoff());
    c[4][1] = static_cast<float>(s.panoramas % 64);   // the march's jitter changes per panorama
    // Blended into the newest complete panorama, so the jitter averages out (RaClouds.fx).
    IDirect3DTexture9* const history = s.panoNew >= 0 ? s.pano[s.panoNew] : 0;
    c[4][2] = history ? kPanoHistoryWeight : 1.0f;
    c[4][3] = static_cast<float>(kPanoH);   // the target's height
    float atmosphere[16][4];
    rendererAtmosphereConstants(atmosphere);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaClouds"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaAtmosphere"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(atmosphere), 16);
    bindMarchTextures(d3dx, v, history);
    bindAirTextures(d3dx);
    const unsigned int rows = kPanoH / kPanoBands;
    drawRows(effect, s.pano[s.panoDraw], kPanoW, s.panoBand * rows, rows);

    if (++s.panoBand == kPanoBands)
    {
        s.panoBand = 0;
        s.panoOld = s.panoNew < 0 ? s.panoDraw : s.panoNew;
        s.panoNew = s.panoDraw;
        if (++s.panoramas == 1)
            std::fprintf(stderr, "[CLOUDS] first panorama drawn\n");
    }
}


// One band of the cloud shadow map (task 8); a finished one goes live. A new one starts
// kShadowSeconds after the last, centred on where the camera's sun ray meets the reference plane.
void shadowStep(void* effect)
{
    State& s = st();
    const AtmosphereInfo a = rendererAtmosphereInfo();
    const double rise = std::max(static_cast<double>(a.toSun[1]), kSunRiseMin);
    if (s.shadowBand == 0)
    {
        if (s.shadowLive >= 0 && secondsSince(s.shadowStarted) < kShadowSeconds)
            return;
        for (int i = 0; i < 2; ++i)
            if (!createTarget(kShadowSize, kShadowSize, D3DFMT_A16B16G16R16F, s.shadow[i]))
                return;
        s.shadowDraw = s.shadowLive == 0 ? 1 : 0;
        s.shadowSet = s.live;
        s.shadowInputs = s.liveInputs;
        s.shadowSeconds = secondsSince(s.start);
        QueryPerformanceCounter(&s.shadowStarted);
        float camera[3];
        cameraPosition(camera);
        const double texel = kShadowWorld / kShadowSize;
        const double up = camera[1] - a.refHeight;
        const double cx = camera[0] - up * a.toSun[0] / rise, cz = camera[2] - up * a.toSun[2] / rise;
        s.shadowCorner[0] = static_cast<float>(std::floor(cx / texel) * texel - 0.5 * kShadowWorld);
        s.shadowCorner[1] = static_cast<float>(std::floor(cz / texel) * texel - 0.5 * kShadowWorld);
    }
    const VolumeSet& v = s.sets[s.shadowSet];
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    if (!d3dx || !v.field || !effectSetTechniqueByName(effect, "RaCloudShadowBuild"))
        return;
    float c[kRows][4];
    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    fillConstants(c, s.shadowInputs, 0);
    fillPanoramaConstants(c, s.shadowInputs, s.shadowSeconds, origin, 0.0f);
    c[8][0] = s.shadowCorner[0];
    c[8][1] = s.shadowCorner[1];
    c[8][2] = a.refHeight;
    c[8][3] = static_cast<float>(kShadowWorld);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaClouds"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
    bindMarchTextures(d3dx, v, 0);
    const unsigned int rows = kShadowSize / kShadowBands;
    drawRows(effect, s.shadow[s.shadowDraw], kShadowSize, s.shadowBand * rows, rows);
    if (++s.shadowBand == kShadowBands)
    {
        s.shadowBand = 0;
        s.shadowLive = s.shadowDraw;
        s.liveShadowInputs = s.shadowInputs;
        s.liveShadowSeconds = s.shadowSeconds;
        s.liveShadowCorner[0] = s.shadowCorner[0];
        s.liveShadowCorner[1] = s.shadowCorner[1];
        if (++s.shadowMaps == 1)
            std::fprintf(stderr, "[CLOUDS] first shadow map drawn\n");
    }
}

float halfToFloat(unsigned short h)
{
    const unsigned int sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
    float v;
    if (exponent == 0)
        v = std::ldexp(static_cast<float>(mantissa), -24);
    else if (exponent == 31)
        v = mantissa ? 0.0f : 65504.0f;
    else
        v = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    return sign ? -v : v;
}

// The sky light's ground panorama (task 7): drawn when due, read back kSkyLightReadDelay frames
// later (so the copy does not wait on the draw) and handed to the atmosphere.
void skyLightStep(void* effect)
{
    State& s = st();
    if (s.skyStage == 0)
    {
        if (!s.skyDue && secondsSince(s.skyLast) < kSkyLightSeconds)
            return;
        if (!createTarget(kSkyLightW, kSkyLightH, D3DFMT_A16B16G16R16F, s.skyPano))
            return;
        const VolumeSet& v = s.sets[s.live];
        ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
        if (!d3dx || !v.field || !effectSetTechniqueByName(effect, "RaCloudPanorama"))
            return;
        const AtmosphereInfo a = rendererAtmosphereInfo();
        // From the level's origin, 2 m over the reference height: the sky every surface sees.
        const float observer[3] = { 0.0f, a.refHeight + 2.0f, 0.0f };
        float c[kRows][4];
        fillConstants(c, s.liveInputs, 0);
        fillPanoramaConstants(c, s.liveInputs, secondsSince(s.start), observer, 0.0f);
        c[4][1] = 0.0f;                                  // jitter seed
        c[4][2] = 1.0f;                                  // no history
        c[4][3] = static_cast<float>(kSkyLightH);        // the target's height
        float atmosphere[16][4];
        rendererAtmosphereConstants(atmosphere);
        if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaClouds"))
            d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
        if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaAtmosphere"))
            d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(atmosphere), 16);
        bindMarchTextures(d3dx, v, 0);
        bindAirTextures(d3dx);
        drawRows(effect, s.skyPano, kSkyLightW, 0, kSkyLightH);
        s.skyStage = 1;
        s.skyDue = false;
        QueryPerformanceCounter(&s.skyLast);
        return;
    }
    if (++s.skyStage <= kSkyLightReadDelay)
        return;
    s.skyStage = 0;
    if (!s.skySys && device()->CreateOffscreenPlainSurface(kSkyLightW, kSkyLightH, D3DFMT_A16B16G16R16F,
                                                           D3DPOOL_SYSTEMMEM, &s.skySys, 0) < 0)
    {
        s.skySys = 0;
        return;
    }
    IDirect3DSurface9* source = 0;
    s.skyPano->GetSurfaceLevel(0, &source);
    const bool copied = source && device()->GetRenderTargetData(source, s.skySys) >= 0;
    if (source)
        source->Release();
    D3DLOCKED_RECT locked;
    if (!copied || s.skySys->LockRect(&locked, 0, D3DLOCK_READONLY) < 0)
        return;
    s.skyData.resize(kSkyLightW * kSkyLightH * 4);
    for (unsigned int y = 0; y < kSkyLightH; ++y)
    {
        const unsigned short* const row = reinterpret_cast<const unsigned short*>(
            static_cast<const unsigned char*>(locked.pBits) + y * locked.Pitch);
        for (unsigned int x = 0; x < kSkyLightW; ++x)
        {
            float* const out = &s.skyData[(y * kSkyLightW + x) * 4];
            const float r = halfToFloat(row[x * 4 + 0]), g = halfToFloat(row[x * 4 + 1]), b = halfToFloat(row[x * 4 + 2]);
            const float t = halfToFloat(row[x * 4 + 3]);
            // Ra_CloudDecompress (RaCloudPanorama.fxh, contract 28): luminance per unit opacity
            const float ratio = (0.2126f * r + 0.7152f * g + 0.0722f * b) / std::max(1.0f - t, 1e-4f);
            const float k = 1.0f / (1.0f - std::min(ratio, 0.98f));
            out[0] = r * k;
            out[1] = g * k;
            out[2] = b * k;
            out[3] = t;
        }
    }
    s.skySys->UnlockRect();
    rendererAtmosphereCloudSky(&s.skyData[0], kSkyLightW, kSkyLightH);
    s.skyHanded = true;
    if (++s.skyLights == 1)
        std::fprintf(stderr, "[CLOUDS] first sky light read back\n");
}

// The live shadow map on the reserved sampler (0 when there is none).
void bindShadowSampler()
{
    State& s = st();
    IDirect3DTexture9* const map = s.enabled && s.shadowLive >= 0 ? s.shadow[s.shadowLive] : 0;
    IDirect3DDevice9* const d = device();
    d->SetTexture(kCloudShadowSampler, map);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(kCloudShadowSampler, D3DSAMP_SRGBTEXTURE, FALSE);
}

} // anonymous namespace

bool& rendererCloudsEnabled() { return st().enabled; }

void rendererCloudsBindSky(void* d3dxEffect)
{
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(d3dxEffect);
    const D3DXHANDLE h = d3dx ? d3dx->GetParameterBySemantic(0, "RaSkyClouds") : 0;
    if (!h)
        return;
    State& s = st();
    D3DXVECTOR4 v(0.0f, 0.0f, 0.0f, 0.0f);
    if (s.enabled && s.panoNew >= 0 && s.pano[s.panoNew] && s.pano[s.panoOld])
    {
        // The newest fades in while the next is drawn: full by its last band.
        v.x = static_cast<float>(s.panoBand) / (kPanoBands - 1);
        v.y = 1.0f;
        // How far the camera is under the slab (contract 16): 1 below its base, 0 above its top.
        float camera[3];
        cameraPosition(camera);
        const double base = rendererAtmosphereInfo().refHeight + s.liveInputs.slabBase();
        const double top = rendererAtmosphereInfo().refHeight + s.liveInputs.slabTop();
        v.z = static_cast<float>(std::min(std::max((top - camera[1]) / std::max(top - base, 1.0), 0.0), 1.0));
        if (const D3DXHANDLE t = d3dx->GetParameterBySemantic(0, "RaCloudPanoramaOld"))
            d3dx->SetTexture(t, s.pano[s.panoOld]);
        if (const D3DXHANDLE t = d3dx->GetParameterBySemantic(0, "RaCloudPanoramaNew"))
            d3dx->SetTexture(t, s.pano[s.panoNew]);
    }
    d3dx->SetVector(h, &v);
}

// The near clouds (task 4): march, resolve against the history, composite over the bound scene.
void rendererCloudsNear(bool allowed)
{
    State& s = st();
    IDirect3DTexture9* const sceneDepth = rendererLinearDepthTexture();
    float fog[3] = { 1.0f, 1.0f, 1.0f };
    postProductionGetFogColor255(fog);
    D3DSURFACE_DESC depthDesc;
    // Thermal views mark themselves by a fog colour with no red (RaCommon.fxh, Ra_IsTisActive).
    if (!allowed || !device() || !s.enabled || s.live < 0 || s.nearDistance <= 0.0f || !sceneDepth
        || fog[0] == 0.0f || sceneDepth->GetLevelDesc(0, &depthDesc) < 0)
    {
        s.nearLast = -1;
        return;
    }
    const unsigned int w = (depthDesc.Width + 1) / 2, h = (depthDesc.Height + 1) / 2;
    if (w != s.nearW || h != s.nearH)
    {
        release(s.nearCurrent);
        release(s.nearDepth);
        release(s.nearHistory[0]);
        release(s.nearHistory[1]);
        s.nearW = w;
        s.nearH = h;
        s.nearLast = -1;
    }
    if (!createTarget(w, h, D3DFMT_A16B16G16R16F, s.nearCurrent) || !createTarget(w, h, D3DFMT_A16B16G16R16F, s.nearDepth)
        || !createTarget(w, h, D3DFMT_A16B16G16R16F, s.nearHistory[0])
        || !createTarget(w, h, D3DFMT_A16B16G16R16F, s.nearHistory[1]))
    {
        s.nearLast = -1;
        return;
    }
    void* const effect = shaderManagerGetEffect(g_shaderManager, "Shaders/RaClouds");
    ID3DXEffect* const d3dx = effect ? static_cast<ID3DXEffect*>(effectD3dxHandle(effect)) : 0;
    if (!d3dx)
    {
        if (effect)
            effectRelease(static_cast<Effect*>(effect));
        s.nearLast = -1;
        return;
    }

    // Constants: the volumes', the observer (the camera now), the view and the previous view.
    const VolumeSet& v = s.sets[s.live];
    const AtmosphereInfo a = rendererAtmosphereInfo();
    IRenderView* const view = rendererGetRenderView();
    const float* const m = view->getViewMatrixPtr()->m;
    const float* const p = view->getProjectionMatrixPtr()->m;
    const float sx = 1.0f / p[0], sy = 1.0f / p[5];
    float c[kRows][4];
    fillConstants(c, s.liveInputs, 0);
    const float camera[3] = { m[12], m[13], m[14] };
    fillPanoramaConstants(c, s.liveInputs, secondsSince(s.start), camera, s.nearDistance);
    const int next = s.nearLast == 0 ? 1 : 0;
    float driftDelta[2] = { 0.0f, 0.0f };
    for (int k = 0; k < 2; ++k)
    {
        // The drift wraps with the world: the step since last frame is the short way round.
        double d = c[5][k] - s.nearPrevDrift[k];
        d -= kWorldSize * std::floor(d / kWorldSize + 0.5);
        driftDelta[k] = static_cast<float>(d);
        s.nearPrevDrift[k] = c[5][k];
    }
    c[9][0] = static_cast<float>(w);
    c[9][1] = static_cast<float>(h);
    c[9][2] = 1.0f / w;
    c[9][3] = 1.0f / h;
    c[10][0] = static_cast<float>(s.nearFrames % 64);
    c[10][1] = kNearHistoryWeight;
    c[10][2] = s.nearLast >= 0 ? 1.0f : 0.0f;
    c[11][0] = static_cast<float>(depthDesc.Width);
    c[11][1] = static_cast<float>(depthDesc.Height);
    c[11][2] = 1.0f / depthDesc.Width;
    c[11][3] = 1.0f / depthDesc.Height;
    for (int k = 0; k < 3; ++k)
    {
        c[12][k] = m[8 + k] - m[0 + k] * sx + m[4 + k] * sy;
        c[13][k] = 2.0f * m[0 + k] * sx;
        c[14][k] = -2.0f * m[4 + k] * sy;
    }
    std::memcpy(c[15], s.nearPrevRows, sizeof(s.nearPrevRows));
    c[18][0] = driftDelta[0];
    c[18][1] = driftDelta[1];
    // This view for the next frame: x' = right . (P - camera) P11, y' = up . (P - camera) P22,
    // w = forward . (P - camera).
    const float scale[3] = { p[0], p[5], 1.0f };
    for (int r = 0; r < 3; ++r)
    {
        const float* const axis = m + 4 * r;
        s.nearPrevRows[r][3] = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            s.nearPrevRows[r][k] = axis[k] * scale[r];
            s.nearPrevRows[r][3] -= axis[k] * camera[k] * scale[r];
        }
    }
    ++s.nearFrames;

    float atmosphere[16][4];
    rendererAtmosphereConstants(atmosphere);
    if (const D3DXHANDLE hc = d3dx->GetParameterBySemantic(0, "RaClouds"))
        d3dx->SetVectorArray(hc, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
    if (const D3DXHANDLE hc = d3dx->GetParameterBySemantic(0, "RaAtmosphere"))
        d3dx->SetVectorArray(hc, reinterpret_cast<const D3DXVECTOR4*>(atmosphere), 16);
    struct { const char* semantic; IDirect3DBaseTexture9* texture; } const textures[] = {
        { "RaCloudShape", s.shape }, { "RaCloudDetail", s.detail }, { "RaCloudField", v.field },
        { "RaCloudCoarse", v.coarse }, { "RaCloudLight", v.light }, { "RaCloudLightTable", a.cloudLight },
        { "RaSkyViewTable", a.skyView },
    };
    for (size_t i = 0; i < sizeof(textures) / sizeof(textures[0]); ++i)
        if (const D3DXHANDLE hc = d3dx->GetParameterBySemantic(0, textures[i].semantic))
            d3dx->SetTexture(hc, textures[i].texture);
    bindAirTextures(d3dx);
    const D3DXHANDLE current = d3dx->GetParameterBySemantic(0, "RaCloudNearCurrent");
    const D3DXHANDLE depth = d3dx->GetParameterBySemantic(0, "RaCloudNearDepth");
    const D3DXHANDLE history = d3dx->GetParameterBySemantic(0, "RaCloudNearHistory");

    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    IDirect3DSurface9* savedDepth = stateManagerGetDepthStencilSurface();
    if (!savedTarget)
        savedTarget = rendererSceneColorSurface();
    if (savedTarget)
        savedTarget->AddRef();
    if (savedDepth)
        savedDepth->AddRef();
    D3DVIEWPORT9 savedViewport;
    device()->GetViewport(&savedViewport);
    stateManagerPush();
    stateManagerSetDepthStencilSurface(0);

    const D3DVIEWPORT9 nearViewport = { 0, 0, w, h, 0.0f, 1.0f };
    auto pass = [&](const char* technique) {
        if (!effectSetTechniqueByName(effect, technique))
            return false;
        effectBegin(effect);
        effectBeginPass(effect, 0);
        geom::fsQuadDrawerDrawCurrentEffect();
        effectEnd(effect);
        return true;
    };
    auto surfaceOf = [](IDirect3DTexture9* t) {
        IDirect3DSurface9* surface = 0;
        t->GetSurfaceLevel(0, &surface);
        return surface;
    };

    // The march: light and transmittance, and the distances.
    IDirect3DSurface9* const currentSurface = surfaceOf(s.nearCurrent);
    IDirect3DSurface9* const depthSurface = surfaceOf(s.nearDepth);
    stateManagerSetRenderTarget(0, currentSurface);
    stateManagerSetRenderTarget(1, depthSurface);
    flushRenderTargetsSeam();
    device()->SetViewport(&nearViewport);
    bool ok = pass("RaCloudNear");
    stateManagerSetRenderTarget(1, 0);

    // The resolve into the other history.
    IDirect3DSurface9* const historySurface = surfaceOf(s.nearHistory[next]);
    if (ok)
    {
        if (current)
            d3dx->SetTexture(current, s.nearCurrent);
        if (depth)
            d3dx->SetTexture(depth, s.nearDepth);
        if (history)
            d3dx->SetTexture(history, s.nearLast >= 0 ? s.nearHistory[s.nearLast] : s.nearCurrent);
        stateManagerSetRenderTarget(0, historySurface);
        flushRenderTargetsSeam();
        device()->SetViewport(&nearViewport);
        ok = pass("RaCloudNearResolve");
    }

    // The composite over the scene, in its own target and viewport.
    stateManagerSetRenderTarget(0, savedTarget);
    stateManagerSetDepthStencilSurface(savedDepth);
    flushRenderTargetsSeam();
    device()->SetViewport(&savedViewport);
    if (ok)
    {
        if (current)
            d3dx->SetTexture(current, s.nearHistory[next]);
        ok = pass("RaCloudNearComposite");
    }
    if (current)
        d3dx->SetTexture(current, 0);
    if (depth)
        d3dx->SetTexture(depth, 0);
    if (history)
        d3dx->SetTexture(history, 0);
    s.nearLast = ok ? next : -1;

    stateManagerPop();
    stateManagerSetRenderTarget(0, savedTarget);
    stateManagerSetDepthStencilSurface(savedDepth);
    flushRenderTargetsSeam();
    device()->SetViewport(&savedViewport);
    for (IDirect3DSurface9* surface : { currentSurface, depthSurface, historySurface, savedTarget, savedDepth })
        if (surface)
            surface->Release();
    effectRelease(static_cast<Effect*>(effect));
}

float& rendererCloudNearDistance() { return st().nearDistance; }

float& rendererCloudCoverage() { return st().coverage; }
int& rendererCloudPreset() { return st().preset; }
float& rendererCloudBase() { return st().baseM; }
float& rendererCloudTop() { return st().topM; }
float& rendererCloudMidBase() { return st().midBaseM; }
float& rendererCloudMidTop() { return st().midTopM; }
float& rendererCloudMidDensity() { return st().midDensity; }
float& rendererCloudMidCoverage() { return st().midCoverage; }
void rendererCloudSettingsChanged() { st().settingsChanged = true; }

bool rendererCloudsSettling()
{
    const State& s = st();
    return s.enabled && (s.settingsChanged || (s.changeBuild && s.stage != kIdle));
}

void rendererCloudsFrame()
{
    State& s = st();
    if (!device())
        return;
    struct BindShadow   // at every return below
    {
        ~BindShadow() { bindShadowSampler(); }
    } bindShadow;
    if (!s.enabled && s.skyHanded)
    {
        rendererAtmosphereCloudSky(0, 0, 0);   // the clear sky's light again
        s.skyHanded = false;
        s.skyDue = true;
    }
    if (!s.enabled || !rendererAtmosphereInfo().fitted)
        return;
    if (!s.started)
    {
        QueryPerformanceCounter(&s.start);
        s.started = true;
    }
    if (!s.noiseStarted)
        startNoise();
    if (!s.noiseReady)
    {
        if (s.noiseThread.joinable() && s.noiseDone)
        {
            s.noiseThread.join();
            s.noiseReady = uploadVolume(s.shape, s.shapeData, kShapeXz, kShapeXz, kShapeY)
                        && uploadVolume(s.detail, s.detailData, kDetailSize, kDetailSize, kDetailSize);
            std::vector<unsigned char>().swap(s.shapeData);
            std::vector<unsigned char>().swap(s.detailData);
        }
        if (!s.noiseReady)
            return;
    }

    // The volumes: start a build when due; a GPU step while one is under way.
    if (s.stage == kIdle)
    {
        const bool due = s.live < 0 || s.settingsChanged || secondsSince(s.lastBuild) >= kRebuildSeconds;
        if (due && ensureSet(s.sets[0]) && ensureSet(s.sets[1]))
        {
            s.changeBuild = s.settingsChanged || s.live < 0;
            s.settingsChanged = false;
            WeatherInputs w;
            w.preset = std::min(std::max(s.preset, 0), 1);
            w.coverage = std::min(std::max(static_cast<double>(s.coverage), 0.0), 1.0);
            const Preset& p = kPresets[w.preset];
            w.baseM = s.baseM > 0.0f ? s.baseM : p.baseM;
            w.topM = std::max(s.topM > 0.0f ? s.topM : p.topM, w.baseM + 500.0);
            w.midBaseM = s.midBaseM > 0.0f ? s.midBaseM : p.midBaseM;
            w.midTopM = std::max(s.midTopM > 0.0f ? s.midTopM : p.midTopM, w.midBaseM + 500.0);
            w.midDensity = s.midDensity >= 0.0f ? s.midDensity : p.midDensity;
            w.midCoverage = std::min(s.midCoverage >= 0.0f ? s.midCoverage : w.coverage * p.midCoverage, 1.0);
            w.seconds = secondsSince(s.start);
            s.built = w;
            s.weatherDone = false;
            s.weatherThread = std::thread([w]() {
                State& t = st();
                LARGE_INTEGER t0;
                QueryPerformanceCounter(&t0);
                buildWeather(w, t.weatherData);
                t.weatherMs = secondsSince(t0) * 1000.0;
                t.weatherDone = true;
            });
            s.stage = kWeather;
            s.buildFrames = 0;
            QueryPerformanceCounter(&s.lastBuild);
        }
    }
    else if (s.stage == kWeather)
    {
        ++s.buildFrames;
        if (s.weatherDone)
        {
            s.weatherThread.join();
            // The live set's field was drawn from the old weather and no longer reads it.
            if (!ensureSet(s.sets[0]) || !ensureSet(s.sets[1]) || !uploadWeather(s.weatherData))
                s.stage = kIdle;
            else
            {
                s.stage = kField;
                s.next = 0;
            }
        }
    }
    const bool building = s.stage == kField || s.stage == kCoarse || s.stage == kLight;
    const bool panorama = s.live >= 0;
    if (!building && !panorama)
        return;

    void* const effect = shaderManagerGetEffect(g_shaderManager, "Shaders/RaClouds");
    if (!effect || !effectD3dxHandle(effect))
    {
        if (!s.effectMissing)
            std::fprintf(stderr, "[CLOUDS] Shaders/RaClouds not loaded: no cloud volumes\n");
        s.effectMissing = true;
        if (effect)
            effectRelease(static_cast<Effect*>(effect));
        return;
    }
    s.effectMissing = false;

    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    IDirect3DSurface9* savedDepth = stateManagerGetDepthStencilSurface();
    if (!savedTarget)
        savedTarget = rendererSceneColorSurface();
    if (savedTarget)
        savedTarget->AddRef();
    if (savedDepth)
        savedDepth->AddRef();
    D3DVIEWPORT9 savedViewport;
    device()->GetViewport(&savedViewport);
    stateManagerPush();
    stateManagerSetDepthStencilSurface(0);

    if (building)
        buildStep(effect);
    if (panorama)
    {
        panoramaStep(effect);
        shadowStep(effect);
        skyLightStep(effect);
    }

    stateManagerPop();
    stateManagerSetRenderTarget(0, savedTarget);
    stateManagerSetDepthStencilSurface(savedDepth);
    flushRenderTargetsSeam();
    device()->SetViewport(&savedViewport);
    if (savedTarget)
        savedTarget->Release();
    if (savedDepth)
        savedDepth->Release();
    effectRelease(static_cast<Effect*>(effect));
}

void rendererCloudShadowConstants(float c[2][4])
{
    std::memset(c, 0, sizeof(float) * 8);
    State& s = st();
    if (!s.enabled || s.shadowLive < 0 || !s.shadow[s.shadowLive])
        return;
    const AtmosphereInfo a = rendererAtmosphereInfo();
    const double rise = std::max(static_cast<double>(a.toSun[1]), kSunRiseMin);
    // The wind's drift since the map was drawn: a receiver reads it that much upwind.
    const Preset& p = kPresets[s.liveShadowInputs.preset];
    double dx, dz;
    windDirection(p, dx, dz);
    const double since = secondsSince(s.start) - s.liveShadowSeconds;
    const double inv = 1.0 / kShadowWorld;
    c[0][0] = static_cast<float>(inv);
    c[0][1] = static_cast<float>(-(s.liveShadowCorner[0] + dx * p.windMs * since) * inv);
    c[0][2] = static_cast<float>(-(s.liveShadowCorner[1] + dz * p.windMs * since) * inv);
    c[0][3] = 1.0f;
    c[1][0] = static_cast<float>(a.toSun[0] / rise);
    c[1][1] = static_cast<float>(a.toSun[2] / rise);
    c[1][2] = a.refHeight;
    c[1][3] = rendererSkyCubeCloudLight().sunTransmittance;
}

IDirect3DTexture9* rendererCloudShadowMap()
{
    State& s = st();
    return s.enabled && s.shadowLive >= 0 ? s.shadow[s.shadowLive] : 0;
}

void rendererCloudsRelease()
{
    if (g_state)
        releaseSets();   // render targets; the weather and noise are managed
}

CloudsInfo rendererCloudsInfo()
{
    State& s = st();
    CloudsInfo i;
    std::memset(&i, 0, sizeof(i));
    i.live = s.live >= 0;
    i.weather = s.weather;
    i.shape = s.shape;
    i.detail = s.detail;
    if (s.live >= 0)
    {
        i.field = s.sets[s.live].field;
        i.coarse = s.sets[s.live].coarse;
        i.light = s.sets[s.live].light;
    }
    const WeatherInputs& w = s.live >= 0 ? s.liveInputs : s.built;
    const Preset& p = kPresets[std::min(std::max(w.preset, 0), 1)];
    i.preset = p.name;
    i.coverage = static_cast<float>(w.coverage);
    i.seconds = static_cast<float>(w.seconds);
    i.baseM = static_cast<float>(w.slabBase());
    i.topM = static_cast<float>(w.slabTop());
    i.lowM[0] = static_cast<float>(w.baseM);
    i.lowM[1] = static_cast<float>(w.topM);
    i.midM[0] = static_cast<float>(w.midBaseM);
    i.midM[1] = static_cast<float>(w.midTopM);
    i.midDensity = static_cast<float>(w.midDensity);
    i.midCoverage = static_cast<float>(w.midCoverage);
    i.lwc = static_cast<float>(p.lwc);
    i.dropletUm = static_cast<float>(p.dropletUm);
    i.noiseMs = static_cast<float>(s.noiseMs);
    i.weatherMs = static_cast<float>(s.weatherMs);
    i.buildFrames = s.lastBuildFrames;
    i.builds = s.builds;
    i.panorama = s.panoNew >= 0 ? s.pano[s.panoNew] : 0;
    i.panoramas = s.panoramas;
    i.nearResolved = s.nearLast >= 0 ? s.nearHistory[s.nearLast] : 0;
    i.nearDistance = s.nearDistance;
    i.shadow = s.shadowLive >= 0 ? s.shadow[s.shadowLive] : 0;
    i.shadowCorner[0] = s.liveShadowCorner[0];
    i.shadowCorner[1] = s.liveShadowCorner[1];
    i.shadowSize = static_cast<float>(kShadowWorld);
    i.shadowMaps = s.shadowMaps;
    i.skyLights = s.skyLights;
    return i;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

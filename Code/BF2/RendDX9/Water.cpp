// [RENDER] Water's waves from an ocean spectrum (render milestone 16, task 2,
// docs/render/water.md#16-water). Not part of retail BF2, whose water tiled one 3D normal volume.
//
// Tessendorf's FFT ocean ("Simulating Ocean Water", 2001) with the JONSWAP spectrum and
// Mitsuyasu's directional spreading (after Horvath, "Empirical directional wave spectra for
// computer graphics", 2015). Three cascades of 256 x 256 frequencies over square patches whose
// sides do not divide each other (kPatch), each holding its own band of wavenumbers so no wave is
// counted twice; summed by the water shader, their periods do not line up into a visible tile.
//
// The initial amplitudes h0(k) and conj(h0(-k)) are built here on the CPU (a worker thread)
// whenever the wind changes, into a 768 x 256 A32B32G32R32F texture, one cascade per 256 columns.
// Every frame, by pixel passes of Shaders/RaWater (shader contract 17):
//
//   RaWaterSpectrum  h(k, t) from h0 and the dispersion (deep water, frequencies quantised to
//                    whole cycles of kLoopSeconds so time wraps without a jump), packed as two
//                    complex fields whose inverse transforms are real: (slope x, slope z) and
//                    (Jxx, Jzz), the horizontal displacement's derivatives; a fourth block of
//                    columns holds the cross derivative Jxz of all three cascades (contract 18)
//   RaWaterFft       8 horizontal then 8 vertical radix-2 Stockham passes, ping-ponging two
//                    1024 x 256 A32B32G32R32F targets
//   RaWaterSlopes    per cascade into its own 256 x 256 A16B16G16R16F target with a full mip chain:
//                    the slopes along and across the wind and their squares (su, sc, su^2, sc^2);
//                    the mips are box-filtered by StretchRect, so a mip's (b, a) minus its (r, g)
//                    squared is the slope variance the texel averaged away (LEAN mapping, task 3).
//                    In the wind's frame the spectrum is symmetric, the slopes' covariance zero, so
//                    four channels hold the whole second moment. The finest cascade also carries
//                    the slope variance of the waves shorter than its grid (below 13.5 cm), as
//                    much as tops the resolved variance up to Cox and Munk's measured sun glitter
//                    statistics for the wind ("Measurement of the roughness of the sea surface from
//                    photographs of the sun's glitter", 1954): sigma_u^2 = 0.00316 U, sigma_c^2 =
//                    0.003 + 0.00192 U, at least kTailFloor of them
//   RaWaterFoam      per foam cascade (the two finer ones; the coarsest is swell, which does not
//                    break) into its own 256 x 256 G16R16F target with mips, ping-ponged with last
//                    frame's (render milestone 16, task 5, contract 18): the surface's compression
//                    1 - J (J the Jacobian of the displaced surface, Tessendorf's x + lambda D,
//                    lambda kChoppiness) past a threshold makes fresh foam, and last frame's fades
//                    over kFoamSeconds. To first order the compression is the displacement's
//                    divergence, whose variance is the cascade's mean square slope; the threshold
//                    is the Gaussian quantile that whitecaps start on a fraction of the surface such
//                    that, with their fade, the foam covers Monahan's measured whitecap coverage for
//                    the wind (3.84e-6 U^3.41; Monahan and O'Muircheartaigh 1980). The mips average
//                    coverage, so distant foam filters to its share of the pixel. The maps' g is the
//                    compression over the ramp, by which the water shader weights the finer
//                    cascade's foam (short waves break on the longer ones' crests).
//
// The three slope maps are bound to the reserved samplers s6-s8 (16x anisotropic, wrap) while the
// water queue draws, and RaWaterSsr.w is set to 1 (rendererWaterBindWaves). The wind's direction is
// the level's WaterScroll (retail's texture scroll), its speed and fetch renderer.waterWind and
// renderer.waterFetch. The foam maps go to s4-s5 (kWaterFoamSampler) and RaWaterFoam (c188) is set
// (renderer.waterFoam scales the coverage; 0 none).
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../Geom/Terrain.h"
#include "../GeomMisc/FSQuadDrawer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;
extern void* g_shaderManager;
void* effectD3dxHandle(void* effect);   // Effect.cpp

namespace {

const unsigned int kN = 256;                      // frequencies per side, per cascade
const unsigned int kLogN = 8;
const unsigned int kCascades = 3;
const double kPatch[kCascades] = { 503.0, 87.0, 17.3 };   // m; the shader's RA_WATER_PATCH
const double kBandSplit = 6.0;      // a cascade starts at this many of its fundamentals
const double kGravity = 9.81;
const double kLoopSeconds = 1000.0; // every frequency a whole number of cycles in this
const double kPi = 3.14159265358979323846;
const unsigned int kRows = 4;       // RaWater's float4s
const double kTailFloor = 0.25;     // the unresolved slope variance: at least this much of Cox-Munk's
const unsigned int kBlocks = kCascades + 1;   // the FFT atlas's blocks of 256 columns
const unsigned int kFoamFirst = 1;  // the cascades that carry foam: kFoamFirst .. kCascades - 1
const unsigned int kFoamMaps = kCascades - kFoamFirst;
const double kChoppiness = 1.0;     // lambda: the displacement the Jacobian is taken of
const double kFoamSeconds = 3.0;    // a whitecap's foam fades by 1/e over this
const double kFoamPersist = 4.0;    // foam covers about this many times the area breaking
const double kFoamRamp = 0.5;       // fresh foam ramps in over this many standard deviations
const double kFoamCheckSeconds = 30.0; // the coverage is read back once, this long after a spectrum

struct Wind
{
    double speed, fetch;     // m/s, m
    double dirX, dirZ;       // unit
    bool operator!=(const Wind& o) const
    {
        return speed != o.speed || fetch != o.fetch || dirX != o.dirX || dirZ != o.dirZ;
    }
};

struct State
{
    float windSpeed, fetchKm;
    // CPU-built spectrum.
    std::thread thread;
    std::atomic<bool> done;
    bool building;
    std::vector<float> h0Data;   // 768 x 256 x 4
    Wind built, live;
    bool haveLive;
    double hs, mss;              // the built spectrum's significant height and mean square slope
    double mssU, mssC;           // its slope variance along and across the wind
    float tail[2], liveTail[2];  // the unresolved slope variance along and across the wind (built, live)
    double cascadeMss[kCascades], liveMss[kCascades];   // each cascade's mean square slope (built, live)
    double liveHs;               // the live spectrum's significant height
    float openTable[kWaterOpenTableRows][kWaterOpenTable][4];   // the built spectrum's fetch table
    float foamScale;             // renderer.waterFoam
    double buildMs;
    // GPU.
    IDirect3DTexture9* h0;
    IDirect3DTexture9* fft[2];
    IDirect3DTexture9* slopes[kCascades];
    IDirect3DTexture9* foam[2][kFoamMaps];   // this frame's and last frame's
    int foamCurrent;
    bool foamFresh;              // no history yet (created, reset, or foam off)
    double lastSeconds;
    double liveSince;            // seconds when the live spectrum was taken up
    bool foamChecked;
    float foamThreshold[kFoamMaps], foamRamp[kFoamMaps];
    bool ready;                  // the slope maps hold this frame's waves
    bool foamReady;              // and the foam maps this frame's foam
    bool effectMissing;
    bool logged;
    LARGE_INTEGER start;
    bool started;
};

State* g_state = 0;
State& st()
{
    if (!g_state)
    {
        g_state = new State();
        g_state->windSpeed = 7.0f;
        g_state->fetchKm = 60.0f;
        g_state->foamScale = 1.0f;
        g_state->foamFresh = true;
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

// ---- the spectrum ----

// JONSWAP: the variance density over angular frequency.
double jonswap(double omega, const Wind& w, double& omegaPeak)
{
    const double g = kGravity;
    const double alpha = 0.076 * std::pow(w.speed * w.speed / (w.fetch * g), 0.22);
    omegaPeak = 22.0 * std::pow(g * g / (w.speed * w.fetch), 1.0 / 3.0);
    const double sigma = omega <= omegaPeak ? 0.07 : 0.09;
    const double r = std::exp(-(omega - omegaPeak) * (omega - omegaPeak)
                              / (2.0 * sigma * sigma * omegaPeak * omegaPeak));
    return alpha * g * g / std::pow(omega, 5.0) * std::exp(-1.25 * std::pow(omegaPeak / omega, 4.0))
         * std::pow(3.3, r);
}

// Mitsuyasu's spreading, cos^2s of half the angle to the wind, normalised over the circle.
double spreading(double theta, double omega, double omegaPeak, const Wind& w)
{
    const double peakS = 11.5 * std::pow(omegaPeak * w.speed / kGravity, -2.5);
    const double ratio = omega / omegaPeak;
    const double s = std::min(std::max(peakS * (ratio <= 1.0 ? std::pow(ratio, 5.0) : std::pow(ratio, -2.5)),
                                       0.5), 40.0);
    const double logQ = (2.0 * s - 1.0) * std::log(2.0) + 2.0 * std::lgamma(s + 1.0) - std::lgamma(2.0 * s + 1.0)
                      - std::log(kPi);
    return std::exp(logQ) * std::pow(std::fabs(std::cos(theta * 0.5)), 2.0 * s);
}

// h0 for every cascade: rgba = h0(k), conj(h0(-k)). Also the significant wave height and the
// mean square slope it adds up to (along and across the wind).
void buildSpectrum(const Wind& w, std::vector<float>& out, double& hs, double& mss, double& mssU, double& mssC,
                   double cascadeMss[kCascades])
{
    out.assign(kN * kCascades * kN * 4, 0.0f);
    std::vector<double> re(kN * kN), im(kN * kN);
    std::mt19937 rng(1234567u);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double windAngle = std::atan2(w.dirZ, w.dirX);
    double variance = 0.0, slope = 0.0, slopeU = 0.0;
    for (unsigned int c = 0; c < kCascades; ++c)
    {
        const double slopeBefore = slope;
        const double dk = 2.0 * kPi / kPatch[c];
        const double kLow = c == 0 ? 0.0 : kBandSplit * 2.0 * kPi / kPatch[c];
        const double kHigh = c + 1 < kCascades ? kBandSplit * 2.0 * kPi / kPatch[c + 1] : 1e30;
        for (unsigned int n = 0; n < kN; ++n)
            for (unsigned int m = 0; m < kN; ++m)
            {
                const double kx = (m < kN / 2 ? static_cast<double>(m) : static_cast<double>(m) - kN) * dk;
                const double kz = (n < kN / 2 ? static_cast<double>(n) : static_cast<double>(n) - kN) * dk;
                const double k = std::sqrt(kx * kx + kz * kz);
                const double xr = gauss(rng), xi = gauss(rng);
                double amp = 0.0;
                if (k > 0.0 && k >= kLow && k < kHigh)
                {
                    const double omega = std::sqrt(kGravity * k);
                    double omegaPeak;
                    const double s = jonswap(omega, w, omegaPeak);
                    const double d = spreading(std::atan2(kz, kx) - windAngle, omega, omegaPeak, w);
                    const double sk = s * d * (kGravity / (2.0 * omega)) / k;   // over (kx, kz)
                    const double energy = sk * dk * dk;
                    variance += energy;
                    slope += energy * k * k;
                    const double ku = kx * w.dirX + kz * w.dirZ;
                    slopeU += energy * ku * ku;
                    amp = std::sqrt(energy * 0.5);
                }
                re[n * kN + m] = xr * amp / std::sqrt(2.0);
                im[n * kN + m] = xi * amp / std::sqrt(2.0);
            }
        for (unsigned int n = 0; n < kN; ++n)
            for (unsigned int m = 0; m < kN; ++m)
            {
                const unsigned int mm = (kN - m) % kN, nn = (kN - n) % kN;
                float* const p = &out[(n * kN * kCascades + c * kN + m) * 4];
                p[0] = static_cast<float>(re[n * kN + m]);
                p[1] = static_cast<float>(im[n * kN + m]);
                p[2] = static_cast<float>(re[nn * kN + mm]);
                p[3] = static_cast<float>(-im[nn * kN + mm]);
            }
        cascadeMss[c] = slope - slopeBefore;
    }
    hs = 4.0 * std::sqrt(variance);
    mss = slope;
    mssU = slopeU;
    mssC = slope - slopeU;
}

// The slope variance the grids do not resolve, along and across the wind (see the header).
void tailVariance(const Wind& w, double mssU, double mssC, float tail[2])
{
    const double coxMunkU = 0.00316 * w.speed;
    const double coxMunkC = 0.003 + 0.00192 * w.speed;
    tail[0] = static_cast<float>(std::max(coxMunkU - mssU, kTailFloor * coxMunkU));
    tail[1] = static_cast<float>(std::max(coxMunkC - mssC, kTailFloor * coxMunkC));
}

// The fetch table of the open-water map (render milestone 23, task 2; WaterOpen.cpp): per entry, a
// fetch of 2^(kWaterOpenTableFirst + e) m (at most the level's), the same wind's JONSWAP integrated
// over each cascade's band of wavenumbers (the spreading integrates to 1 over the circle): [0] each
// cascade's slope amplitude against the level's spectrum, sqrt of the ratio of mean square slopes,
// and the significant height's ratio; [1].x the whitecap coverage's ratio from the wave age:
// Zhao and Toba ("Dependence of whitecap coverage on wind and wind-wave properties", 2001) measure
// W proportional to (u*^2 / (nu omega_p))^1.09, and JONSWAP's omega_p goes as fetch^(-1/3), so
// W goes as fetch^0.363 for one wind. Young seas are steeper (JONSWAP's alpha grows as the fetch
// shortens), so the finest cascade's amplitude can pass 1 while the long waves vanish.
void bandMoments(const Wind& w, double variance[kCascades], double mss[kCascades])
{
    const double limit[kCascades + 1] = { 2.0 * kPi / kPatch[0], kBandSplit * 2.0 * kPi / kPatch[1],
                                          kBandSplit * 2.0 * kPi / kPatch[2], 0.5 * kN * 2.0 * kPi / kPatch[2] };
    const int steps = 200;
    for (unsigned int c = 0; c < kCascades; ++c)
    {
        variance[c] = mss[c] = 0.0;
        const double l0 = std::log(limit[c]), l1 = std::log(limit[c + 1]);
        for (int i = 0; i < steps; ++i)
        {
            const double k = std::exp(l0 + (i + 0.5) * (l1 - l0) / steps);
            const double dk = k * (l1 - l0) / steps;
            const double omega = std::sqrt(kGravity * k);
            double omegaPeak;
            const double sk = jonswap(omega, w, omegaPeak) * 0.5 * omega / k;   // dω/dk = ω / 2k
            variance[c] += sk * dk;
            mss[c] += sk * k * k * dk;
        }
    }
}

void openTable(const Wind& w, float table[kWaterOpenTableRows][kWaterOpenTable][4])
{
    double var0[kCascades], mss0[kCascades];
    bandMoments(w, var0, mss0);
    const double total0 = var0[0] + var0[1] + var0[2];
    for (int e = 0; e < kWaterOpenTable; ++e)
    {
        Wind local = w;
        local.fetch = std::min(std::ldexp(1.0, kWaterOpenTableFirst + e), w.fetch);
        double var[kCascades], mss[kCascades];
        bandMoments(local, var, mss);
        for (unsigned int c = 0; c < kCascades; ++c)
            table[0][e][c] = static_cast<float>(mss0[c] > 0.0 ? std::sqrt(mss[c] / mss0[c]) : 1.0);
        table[0][e][3] = static_cast<float>(total0 > 0.0 ? std::sqrt((var[0] + var[1] + var[2]) / total0) : 1.0);
        table[1][e][0] = static_cast<float>(std::pow(local.fetch / w.fetch, 1.09 / 3.0));
        table[1][e][1] = table[1][e][2] = table[1][e][3] = 0.0f;
    }
}

// The upper tail of the unit normal distribution, and its inverse (by bisection).
double gaussTail(double z) { return 0.5 * std::erfc(z / std::sqrt(2.0)); }
double gaussTailInverse(double p)
{
    double lo = -8.0, hi = 8.0;
    for (int i = 0; i < 60; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (gaussTail(mid) > p ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

// Monahan's whitecap coverage for the wind, as a fraction.
double monahanCoverage(double speed)
{
    return std::min(3.84e-6 * std::pow(speed, 3.41), 0.5);
}

// Each foam cascade's threshold on the compression and 1 / its ramp: whitecaps start on
// coverage / (kFoamPersist x the foam cascades) of each cascade's surface.
void foamThresholds(const Wind& w, const double mss[kCascades], float scale, float threshold[kFoamMaps],
                    float ramp[kFoamMaps])
{
    const double breaking = std::max(monahanCoverage(w.speed) * scale, 1e-7) / (kFoamPersist * kFoamMaps);
    const double z = gaussTailInverse(std::min(breaking, 0.5));
    for (unsigned int i = 0; i < kFoamMaps; ++i)
    {
        const double sigma = kChoppiness * std::sqrt(std::max(mss[kFoamFirst + i], 1e-8));
        threshold[i] = static_cast<float>(z * sigma);
        ramp[i] = static_cast<float>(1.0 / (kFoamRamp * sigma));
    }
}

bool createTarget(unsigned int w, unsigned int h, unsigned int levels, D3DFORMAT format, IDirect3DTexture9*& t)
{
    if (t)
        return true;
    const HRESULT hr = device()->CreateTexture(w, h, levels, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, 0);
    if (hr < 0)
    {
        std::fprintf(stderr, "[WATER] target %ux%u format %d failed: hr=0x%08lX\n", w, h,
                     static_cast<int>(format), static_cast<unsigned long>(hr));
        t = 0;
        return false;
    }
    return true;
}

bool uploadSpectrum()
{
    State& s = st();
    if (!s.h0 && device()->CreateTexture(kN * kCascades, kN, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED,
                                         &s.h0, 0) < 0)
    {
        std::fprintf(stderr, "[WATER] spectrum texture failed\n");
        s.h0 = 0;
        return false;
    }
    D3DLOCKED_RECT r;
    if (s.h0->LockRect(0, &r, 0, 0) < 0)
        return false;
    for (unsigned int y = 0; y < kN; ++y)
        std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, &s.h0Data[y * kN * kCascades * 4],
                    kN * kCascades * 4 * sizeof(float));
    s.h0->UnlockRect(0);
    return true;
}

Wind currentWind()
{
    State& s = st();
    Wind w;
    w.speed = std::min(std::max(static_cast<double>(s.windSpeed), 1.0), 30.0);
    w.fetch = std::min(std::max(static_cast<double>(s.fetchKm), 1.0), 1000.0) * 1000.0;
    w.dirX = 1.0;
    w.dirZ = 0.0;
    if (geom::Terrain* const t = geom::terrain())
    {
        Vec2 v;
        t->getWaterScroll(&v);
        const double len = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y);
        if (len > 1e-6)
        {
            w.dirX = v.x / len;
            w.dirZ = v.y / len;
        }
    }
    return w;
}

void setConstants(ID3DXEffect* d3dx, const float c[kRows][4])
{
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaWater"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(c), kRows);
}

void setInput(ID3DXEffect* d3dx, IDirect3DTexture9* t)
{
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaWaterInput"))
        d3dx->SetTexture(h, t);
}

void drawInto(void* effect, IDirect3DTexture9* target, unsigned int w, unsigned int h)
{
    IDirect3DSurface9* surface = 0;
    target->GetSurfaceLevel(0, &surface);
    stateManagerSetRenderTarget(0, surface);
    flushRenderTargetsSeam();
    const D3DVIEWPORT9 viewport = { 0, 0, w, h, 0.0f, 1.0f };
    device()->SetViewport(&viewport);
    effectBegin(effect);
    effectBeginPass(effect, 0);
    geom::fsQuadDrawerDrawCurrentEffect();
    effectEnd(effect);
    if (surface)
        surface->Release();
}

void setHistory(ID3DXEffect* d3dx, IDirect3DTexture9* t)
{
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaWaterHistory"))
        d3dx->SetTexture(h, t);
}

// Box-filters level 0 down the chain.
void buildMips(IDirect3DTexture9* t)
{
    const DWORD levels = t->GetLevelCount();
    for (DWORD l = 1; l < levels; ++l)
    {
        IDirect3DSurface9* src = 0;
        IDirect3DSurface9* dst = 0;
        t->GetSurfaceLevel(l - 1, &src);
        t->GetSurfaceLevel(l, &dst);
        if (src && dst)
            device()->StretchRect(src, 0, dst, 0, D3DTEXF_LINEAR);
        if (src)
            src->Release();
        if (dst)
            dst->Release();
    }
}

// The waves at `seconds`: spectrum, the inverse transforms, the slope maps and their mips; with
// `foam`, the foam maps `dt` seconds after the last ones.
bool drawWaves(void* effect, double seconds, double dt, bool foam)
{
    State& s = st();
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect));
    if (!d3dx)
        return false;
    float c[kRows][4];
    std::memset(c, 0, sizeof(c));
    c[1][0] = static_cast<float>(std::fmod(seconds, kLoopSeconds));
    c[1][1] = static_cast<float>(kGravity);
    c[1][2] = static_cast<float>(2.0 * kPi / kLoopSeconds);
    for (unsigned int i = 0; i < kCascades; ++i)
        c[2][i] = static_cast<float>(2.0 * kPi / kPatch[i]);
    c[1][3] = static_cast<float>(kChoppiness);

    if (!effectSetTechniqueByName(effect, "RaWaterSpectrum"))
        return false;
    setConstants(d3dx, c);
    setInput(d3dx, s.h0);
    drawInto(effect, s.fft[0], kN * kBlocks, kN);

    if (!effectSetTechniqueByName(effect, "RaWaterFft"))
        return false;
    int from = 0;
    for (unsigned int pass = 0; pass < 2 * kLogN; ++pass)
    {
        c[0][0] = static_cast<float>(1u << (pass % kLogN));   // Ns
        c[0][1] = pass < kLogN ? 1.0f : 0.0f;                 // horizontal, then vertical
        setConstants(d3dx, c);
        setInput(d3dx, s.fft[from]);
        drawInto(effect, s.fft[1 - from], kN * kBlocks, kN);
        from = 1 - from;
    }

    if (!effectSetTechniqueByName(effect, "RaWaterSlopes"))
        return false;
    setInput(d3dx, s.fft[from]);
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        c[0][2] = static_cast<float>(i);
        c[3][0] = static_cast<float>(s.live.dirX);
        c[3][1] = static_cast<float>(s.live.dirZ);
        c[3][2] = i + 1 == kCascades ? s.liveTail[0] : 0.0f;
        c[3][3] = i + 1 == kCascades ? s.liveTail[1] : 0.0f;
        setConstants(d3dx, c);
        drawInto(effect, s.slopes[i], kN, kN);
    }
    if (foam)
    {
        if (!effectSetTechniqueByName(effect, "RaWaterFoam"))
            return false;
        const int previous = s.foamCurrent;
        const int current = 1 - previous;
        c[0][3] = s.foamFresh ? -1.0f : static_cast<float>(std::exp(-dt / kFoamSeconds));
        for (unsigned int i = 0; i < kFoamMaps; ++i)
        {
            c[0][2] = static_cast<float>(kFoamFirst + i);
            c[3][2] = s.foamThreshold[i];
            c[3][3] = s.foamRamp[i];
            setConstants(d3dx, c);
            setHistory(d3dx, s.foam[previous][i]);
            drawInto(effect, s.foam[current][i], kN, kN);
        }
        setHistory(d3dx, 0);
        s.foamCurrent = current;
        s.foamFresh = false;
    }
    setInput(d3dx, 0);
    for (unsigned int i = 0; i < kCascades; ++i)
        buildMips(s.slopes[i]);
    if (foam)
        for (unsigned int i = 0; i < kFoamMaps; ++i)
            buildMips(s.foam[s.foamCurrent][i]);
    return true;
}

// Half to float (the foam maps' format), for the coverage check.
float halfToFloat(unsigned short h)
{
    const unsigned int sign = (h >> 15) & 1u, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    const float m = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                                  : std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    return sign ? -m : m;
}

// The foam's mean coverage: each map's last mip read back (a one-off stall), combined as
// independent layers.
void logFoamCoverage()
{
    State& s = st();
    double clear = 1.0;
    for (unsigned int i = 0; i < kFoamMaps; ++i)
    {
        IDirect3DTexture9* const t = s.foam[s.foamCurrent][i];
        IDirect3DSurface9* top = 0;
        IDirect3DSurface9* mem = 0;
        t->GetSurfaceLevel(t->GetLevelCount() - 1, &top);
        if (top && device()->CreateOffscreenPlainSurface(1, 1, D3DFMT_G16R16F, D3DPOOL_SYSTEMMEM, &mem, 0) >= 0
            && device()->GetRenderTargetData(top, mem) >= 0)
        {
            D3DLOCKED_RECT r;
            if (mem->LockRect(&r, 0, D3DLOCK_READONLY) >= 0)
            {
                clear *= 1.0 - std::min(std::max(static_cast<double>(halfToFloat(*static_cast<unsigned short*>(r.pBits))), 0.0), 1.0);
                mem->UnlockRect();
            }
        }
        if (mem)
            mem->Release();
        if (top)
            top->Release();
    }
    std::fprintf(stderr, "[WATER] foam: covers %.2f %% of the open sea (Monahan at %.1f m/s: %.2f %%, times "
                 "renderer.waterFoam %.2f); thresholds %.4f, %.4f\n", (1.0 - clear) * 100.0, s.live.speed,
                 monahanCoverage(s.live.speed) * 100.0, s.foamScale, s.foamThreshold[0], s.foamThreshold[1]);
}

} // namespace

float& rendererWaterFoam() { return st().foamScale; }
float& rendererWaterWind() { return st().windSpeed; }
float& rendererWaterFetch() { return st().fetchKm; }

void rendererWaterFrame()
{
    State& s = st();
    s.ready = false;
    s.foamReady = false;
    if (!device() || !geom::terrain())
        return;
    if (!s.started)
    {
        QueryPerformanceCounter(&s.start);
        s.started = true;
    }

    // The spectrum: rebuilt on a worker whenever the wind changes; the old one draws meanwhile.
    const Wind wind = currentWind();
    rendererWaterOpenFrame(wind.dirX, wind.dirZ);   // milestone 23: the open-water map
    if (s.building && s.done)
    {
        s.thread.join();
        s.building = false;
        if (uploadSpectrum())
        {
            s.live = s.built;
            s.liveTail[0] = s.tail[0];
            s.liveTail[1] = s.tail[1];
            for (unsigned int i = 0; i < kCascades; ++i)
                s.liveMss[i] = s.cascadeMss[i];
            s.liveHs = s.hs;
            rendererWaterOpenTable(s.openTable);
            s.liveSince = secondsSince(s.start);
            s.foamChecked = false;
            s.haveLive = true;
            std::fprintf(stderr, "[WATER] spectrum: wind %.1f m/s toward (%.2f, %.2f), fetch %.0f km: "
                         "significant height %.2f m, mean square slope %.4f (along the wind %.4f, "
                         "across %.4f), unresolved %.4f, %.4f; built in %.0f ms\n",
                         s.live.speed, s.live.dirX, s.live.dirZ, s.live.fetch / 1000.0, s.hs, s.mss,
                         s.mssU, s.mssC, s.liveTail[0], s.liveTail[1], s.buildMs);
        }
        std::vector<float>().swap(s.h0Data);
    }
    if (!s.building && (!s.haveLive || s.live != wind))
    {
        s.built = wind;
        s.done = false;
        s.building = true;
        s.thread = std::thread([wind]() {
            State& t = st();
            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);
            buildSpectrum(wind, t.h0Data, t.hs, t.mss, t.mssU, t.mssC, t.cascadeMss);
            tailVariance(wind, t.mssU, t.mssC, t.tail);
            openTable(wind, t.openTable);
            t.buildMs = secondsSince(t0) * 1000.0;
            t.done = true;
        });
    }
    if (!s.haveLive)
        return;

    bool targets = createTarget(kN * kBlocks, kN, 1, D3DFMT_A32B32G32R32F, s.fft[0])
                && createTarget(kN * kBlocks, kN, 1, D3DFMT_A32B32G32R32F, s.fft[1]);
    for (unsigned int i = 0; i < kCascades && targets; ++i)
        targets = createTarget(kN, kN, 0, D3DFMT_A16B16G16R16F, s.slopes[i]);
    if (!targets)
        return;
    bool foam = s.foamScale > 0.0f;
    for (unsigned int i = 0; i < kFoamMaps && foam; ++i)
    {
        const bool had = s.foam[0][i] && s.foam[1][i];
        foam = createTarget(kN, kN, 0, D3DFMT_G16R16F, s.foam[0][i]) && createTarget(kN, kN, 0, D3DFMT_G16R16F, s.foam[1][i]);
        if (!had)
            s.foamFresh = true;
    }
    if (!foam)
        s.foamFresh = true;
    else
        foamThresholds(s.live, s.liveMss, s.foamScale, s.foamThreshold, s.foamRamp);

    void* const effect = shaderManagerGetEffect(g_shaderManager, "Shaders/RaWater");
    if (!effect || !effectD3dxHandle(effect))
    {
        if (!s.effectMissing)
            std::fprintf(stderr, "[WATER] Shaders/RaWater not loaded: retail water normals\n");
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

    const double seconds = secondsSince(s.start);
    const double dt = std::min(std::max(seconds - s.lastSeconds, 0.0), 0.25);
    s.lastSeconds = seconds;
    s.ready = drawWaves(effect, seconds, dt, foam);
    s.foamReady = s.ready && foam;
    if (s.foamReady && !s.foamChecked && seconds - s.liveSince > kFoamCheckSeconds)
    {
        logFoamCoverage();
        s.foamChecked = true;
    }
    if (s.ready && !s.logged)
    {
        std::fprintf(stderr, "[WATER] first waves drawn\n");
        s.logged = true;
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

bool rendererWaterBindWaves()
{
    State& s = st();
    IDirect3DDevice9* const d = device();
    if (!d || !s.ready)
        return false;
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const DWORD stage = kWaterWaveSampler + i;
        d->SetTexture(stage, s.slopes[i]);
        d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
        d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, 16);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
        d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    }
    // The foam (task 5): x 1 while its maps are bound, y the depth waves break at (the significant
    // height / 0.78), z the seconds of the waves' loop
    float foam[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (s.foamReady)
    {
        for (unsigned int i = 0; i < kFoamMaps; ++i)
        {
            const DWORD stage = kWaterFoamSampler + i;
            d->SetTexture(stage, s.foam[s.foamCurrent][i]);
            d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, 16);
            d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
            d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
            d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        foam[0] = 1.0f;
        foam[1] = static_cast<float>(std::max(s.liveHs / 0.78, 0.3));
        foam[2] = static_cast<float>(std::fmod(s.lastSeconds, kLoopSeconds));
    }
    // The open-water map (milestone 23, contract 38): w 1 / its side in metres while bound
    foam[3] = rendererWaterOpenBind();
    stateManagerSetWaterFoam(foam);
    return true;
}

void rendererWaterUnbindWaves()
{
    if (IDirect3DDevice9* const d = device())
    {
        for (unsigned int i = 0; i < kCascades; ++i)
            d->SetTexture(kWaterWaveSampler + i, 0);
        for (unsigned int i = 0; i < kFoamMaps; ++i)
            d->SetTexture(kWaterFoamSampler + i, 0);
        rendererWaterOpenUnbind();
        static const float kNone[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        stateManagerSetWaterFoam(kNone);
    }
}

void rendererWaterRelease()
{
    if (!g_state)
        return;
    State& s = *g_state;
    release(s.fft[0]);
    release(s.fft[1]);
    for (unsigned int i = 0; i < kCascades; ++i)
        release(s.slopes[i]);
    for (unsigned int i = 0; i < kFoamMaps; ++i)
    {
        release(s.foam[0][i]);
        release(s.foam[1][i]);
    }
    s.foamFresh = true;
    s.ready = false;
    s.foamReady = false;   // the managed spectrum survives a reset
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

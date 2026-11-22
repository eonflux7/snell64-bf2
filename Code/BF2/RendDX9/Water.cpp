// [RENDER] Water's waves from an ocean spectrum (render milestone 16, task 2,
// docs/lanes/render.md#16-water). Not part of retail BF2, whose water tiled one 3D normal volume.
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
//                    (d Dx / dx, d Dz / dz), the horizontal displacement's derivatives (foam, later)
//   RaWaterFft       8 horizontal then 8 vertical radix-2 Stockham passes, ping-ponging two
//                    768 x 256 A32B32G32R32F targets
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
//
// The three slope maps are bound to the reserved samplers s6-s8 (16x anisotropic, wrap) while the
// water queue draws, and RaWaterSsr.w is set to 1 (rendererWaterBindWaves). The wind's direction is
// the level's WaterScroll (retail's texture scroll), its speed and fetch renderer.waterWind and
// renderer.waterFetch.
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
    bool enabled;
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
    double buildMs;
    // GPU.
    IDirect3DTexture9* h0;
    IDirect3DTexture9* fft[2];
    IDirect3DTexture9* slopes[kCascades];
    bool ready;                  // the slope maps hold this frame's waves
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
        g_state->enabled = true;
        g_state->windSpeed = 7.0f;
        g_state->fetchKm = 60.0f;
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
void buildSpectrum(const Wind& w, std::vector<float>& out, double& hs, double& mss, double& mssU, double& mssC)
{
    out.assign(kN * kCascades * kN * 4, 0.0f);
    std::vector<double> re(kN * kN), im(kN * kN);
    std::mt19937 rng(1234567u);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const double windAngle = std::atan2(w.dirZ, w.dirX);
    double variance = 0.0, slope = 0.0, slopeU = 0.0;
    for (unsigned int c = 0; c < kCascades; ++c)
    {
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

// The waves at `seconds`: spectrum, the inverse transforms, the slope maps and their mips.
bool drawWaves(void* effect, double seconds)
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

    if (!effectSetTechniqueByName(effect, "RaWaterSpectrum"))
        return false;
    setConstants(d3dx, c);
    setInput(d3dx, s.h0);
    drawInto(effect, s.fft[0], kN * kCascades, kN);

    if (!effectSetTechniqueByName(effect, "RaWaterFft"))
        return false;
    int from = 0;
    for (unsigned int pass = 0; pass < 2 * kLogN; ++pass)
    {
        c[0][0] = static_cast<float>(1u << (pass % kLogN));   // Ns
        c[0][1] = pass < kLogN ? 1.0f : 0.0f;                 // horizontal, then vertical
        setConstants(d3dx, c);
        setInput(d3dx, s.fft[from]);
        drawInto(effect, s.fft[1 - from], kN * kCascades, kN);
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
    setInput(d3dx, 0);
    for (unsigned int i = 0; i < kCascades; ++i)
    {
        const DWORD levels = s.slopes[i]->GetLevelCount();
        for (DWORD l = 1; l < levels; ++l)
        {
            IDirect3DSurface9* src = 0;
            IDirect3DSurface9* dst = 0;
            s.slopes[i]->GetSurfaceLevel(l - 1, &src);
            s.slopes[i]->GetSurfaceLevel(l, &dst);
            if (src && dst)
                device()->StretchRect(src, 0, dst, 0, D3DTEXF_LINEAR);
            if (src)
                src->Release();
            if (dst)
                dst->Release();
        }
    }
    return true;
}

} // namespace

bool& rendererWaterWavesEnabled() { return st().enabled; }
float& rendererWaterWind() { return st().windSpeed; }
float& rendererWaterFetch() { return st().fetchKm; }

void rendererWaterFrame()
{
    State& s = st();
    s.ready = false;
    if (!device() || !s.enabled || !geom::terrain())
        return;
    if (!s.started)
    {
        QueryPerformanceCounter(&s.start);
        s.started = true;
    }

    // The spectrum: rebuilt on a worker whenever the wind changes; the old one draws meanwhile.
    const Wind wind = currentWind();
    if (s.building && s.done)
    {
        s.thread.join();
        s.building = false;
        if (uploadSpectrum())
        {
            s.live = s.built;
            s.liveTail[0] = s.tail[0];
            s.liveTail[1] = s.tail[1];
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
            buildSpectrum(wind, t.h0Data, t.hs, t.mss, t.mssU, t.mssC);
            tailVariance(wind, t.mssU, t.mssC, t.tail);
            t.buildMs = secondsSince(t0) * 1000.0;
            t.done = true;
        });
    }
    if (!s.haveLive)
        return;

    bool targets = createTarget(kN * kCascades, kN, 1, D3DFMT_A32B32G32R32F, s.fft[0])
                && createTarget(kN * kCascades, kN, 1, D3DFMT_A32B32G32R32F, s.fft[1]);
    for (unsigned int i = 0; i < kCascades && targets; ++i)
        targets = createTarget(kN, kN, 0, D3DFMT_A16B16G16R16F, s.slopes[i]);
    if (!targets)
        return;

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

    s.ready = drawWaves(effect, secondsSince(s.start));
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
    return true;
}

void rendererWaterUnbindWaves()
{
    if (IDirect3DDevice9* const d = device())
        for (unsigned int i = 0; i < kCascades; ++i)
            d->SetTexture(kWaterWaveSampler + i, 0);
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
    s.ready = false;   // the managed spectrum survives a reset
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

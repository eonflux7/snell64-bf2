// [RENDER] The frame's HDR post before the grade (render milestone 13,
// docs/lanes/render.md#13-hdr-bloom--auto-exposure, contract 19). Not part of retail BF2.
//
// Runs in the post-production composite, on the resolved single-sample float scene, just before
// the final copy (FSQuadDrawer pass 0, the frame grade). Every pass is technique of
// Shaders/RaHdrPost, one full-screen pixel pass into a target of its own:
//
//   bloom      levels 0-5, each half the one above (level 0 half the scene), A16B16G16R16F: the
//              scene down the chain (Jimenez's 13-tap filter, level 0 Karis-averaged), then back
//              up, each level's tent added into the one above; level 0 is the bloom the grade
//              mixes in at renderer.bloomStrength
//   meter      64 x 32 A16B16G16R16F, from level 3: log2 luminance, the centre weight
//   rows       16 x 64 A32B32G32R32F: the histogram's partial sums, four bins to a texel
//   histogram  16 x 1 A32B32G32R32F: 64 bins over kEvLow .. kEvLow + kEvRange
//   state      1 x 1 A32B32G32R32F, two in a ping-pong: the adapted log2 luminance, the exposure,
//              the metered log2 luminance, 1 once written
//
// The grade reads the state and the bloom (RaGrade, set here on the FSQuadDrawer effect); particle
// emissives read the state per vertex to undo the exposure (rendererHdrPostBindParticles).
// Thermals (a fog colour with no red, as Clouds.cpp tells them) skip all of it: exposure 1, no
// bloom, the 2.2 encode. A device reset releases the targets; the first frame after it, and a
// frame more than kSnapSeconds after the last (a level load), snaps to the metered value.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "Texture.h"
#include "../GeomMisc/FSQuadDrawer.h"

#include <cmath>
#include <cstdio>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;
extern void* g_shaderManager;
void* effectD3dxHandle(void* effect);   // Effect.cpp

namespace {

const unsigned int kLevels = 6;
const unsigned int kMeterLevel = 3;                  // 1/16 of the scene
const unsigned int kMeterW = 64, kMeterH = 32;       // RaHdrPost.fx's HDR_METER_*
const unsigned int kGroups = 16;                     // 64 bins, four to a texel
const float kEvLow = -14.0f, kEvRange = 24.0f;       // the histogram, log2 luminance
const float kLowPercentile = 0.5f, kHighPercentile = 0.95f;
// The metered luminance shown unchanged (0 stops): Muttrah City's reference views meter -1.0 to -1.3 EV
// (50th-95th percentile), and their look at 0 stops is milestone 11's, accepted; 0.42 is -1.25 EV.
const float kKey = 0.42f;
const float kLuminanceCap = 100.0f;                  // per scene sample, into the bloom
const float kRateLight = 3.0f, kRateDark = 1.0f;     // 1/s, times renderer.exposureSpeed
const double kSnapSeconds = 0.5;

struct State
{
    float bloomStrength = 0.04f;
    float exposureBias = 0.0f;         // stops, over the auto-exposure
    // The ceiling keeps night dark: The Falklands' night layer meters -9 EV and sits on it; at +4
    // its terrain draws at about 25/255 (retail about 30, at +4.5).
    float exposureMin = -4.0f, exposureMax = 4.0f;
    float exposureSpeed = 1.0f;

    unsigned int w = 0, h = 0;          // the scene's size the targets were made for
    unsigned int levelW[kLevels] = {}, levelH[kLevels] = {};
    IDirect3DTexture9* level[kLevels] = {};
    IDirect3DTexture9* meter = 0;
    IDirect3DTexture9* rows = 0;
    IDirect3DTexture9* histogram = 0;
    IDirect3DTexture9* state[2] = {};
    int live = -1;                      // the state written last; -1 none
    bool stateLiveForParticles = false; // the live state holds an auto-exposure

    LARGE_INTEGER last = {};
    unsigned int frames = 0;
    float lastDt = 0.0f;
    bool lastSnap = false;
    bool thermal = false;
    bool ran = false;                   // the passes ran this frame
};

State& st()
{
    static State s;
    return s;
}

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

void release(IDirect3DTexture9*& t)
{
    if (t)
        t->Release();
    t = 0;
}

void releaseAll()
{
    State& s = st();
    for (unsigned int i = 0; i < kLevels; ++i)
        release(s.level[i]);
    release(s.meter);
    release(s.rows);
    release(s.histogram);
    release(s.state[0]);
    release(s.state[1]);
    s.live = -1;
    s.stateLiveForParticles = false;
    s.w = s.h = 0;
}

bool createTarget(unsigned int w, unsigned int h, D3DFORMAT format, IDirect3DTexture9*& t)
{
    if (t)
        return true;
    const HRESULT hr = device()->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t, 0);
    if (hr < 0)
    {
        std::fprintf(stderr, "[HDRPOST] target %ux%u format %d failed: hr=0x%08lX\n", w, h,
                     static_cast<int>(format), static_cast<unsigned long>(hr));
        t = 0;
        return false;
    }
    return true;
}

bool ensureTargets(unsigned int w, unsigned int h)
{
    State& s = st();
    if (w != s.w || h != s.h)
    {
        releaseAll();
        s.w = w;
        s.h = h;
        unsigned int lw = w, lh = h;
        for (unsigned int i = 0; i < kLevels; ++i)
        {
            lw = lw > 1 ? (lw + 1) / 2 : 1;
            lh = lh > 1 ? (lh + 1) / 2 : 1;
            s.levelW[i] = lw;
            s.levelH[i] = lh;
        }
    }
    bool ok = true;
    for (unsigned int i = 0; i < kLevels; ++i)
        ok = ok && createTarget(s.levelW[i], s.levelH[i], D3DFMT_A16B16G16R16F, s.level[i]);
    return ok && createTarget(kMeterW, kMeterH, D3DFMT_A16B16G16R16F, s.meter)
        && createTarget(kGroups, 64, D3DFMT_A32B32G32R32F, s.rows)
        && createTarget(kGroups, 1, D3DFMT_A32B32G32R32F, s.histogram)
        && createTarget(1, 1, D3DFMT_A32B32G32R32F, s.state[0])
        && createTarget(1, 1, D3DFMT_A32B32G32R32F, s.state[1]);
}

void textureSize(IDirect3DTexture9* t, unsigned int& w, unsigned int& h)
{
    D3DSURFACE_DESC desc;
    if (t && t->GetLevelDesc(0, &desc) >= 0)
    {
        w = desc.Width;
        h = desc.Height;
    }
    else
        w = h = 1;
}

bool thermalView()
{
    float fog[3] = { 1.0f, 1.0f, 1.0f };
    postProductionGetFogColor255(fog);
    return fog[0] == 0.0f;   // Ra_IsTisActive (RaCommon.fxh)
}

// One pass of `technique` from `source` into `target`, with RaHdrPost's rows 1-3 in `c` (row 0,
// the sizes, is filled here).
struct Passes
{
    void* effect;
    ID3DXEffect* d3dx;
    D3DXHANDLE constants, source, history;
    float c[4][4];

    bool draw(const char* technique, IDirect3DBaseTexture9* input, unsigned int inW, unsigned int inH,
              IDirect3DTexture9* target)
    {
        unsigned int w, h;
        textureSize(target, w, h);
        c[0][0] = 1.0f / inW;
        c[0][1] = 1.0f / inH;
        c[0][2] = static_cast<float>(w);
        c[0][3] = static_cast<float>(h);
        if (!effectSetTechniqueByName(effect, technique))
            return false;
        if (constants)
            d3dx->SetVectorArray(constants, reinterpret_cast<const D3DXVECTOR4*>(c), 4);
        if (source)
            d3dx->SetTexture(source, input);
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
        return true;
    }
};

double secondsSinceLast(State& s)
{
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    const double dt = (s.last.QuadPart && f.QuadPart)
        ? static_cast<double>(t.QuadPart - s.last.QuadPart) / f.QuadPart : 1e9;
    s.last = t;
    return dt < 0.0 ? 0.0 : dt;
}

// The passes; false when nothing could be drawn (the grade then takes the manual exposure and no
// bloom).
bool runPasses(EngineTexture* scene)
{
    State& s = st();
    IDirect3DBaseTexture9* const sceneTexture = scene ? scene->baseTexture : 0;
    if (!device() || !sceneTexture || !scene->width || !scene->height)
        return false;
    if (!ensureTargets(scene->width, scene->height))
        return false;
    void* const effect = shaderManagerGetEffect(g_shaderManager, "Shaders/RaHdrPost");
    ID3DXEffect* const d3dx = effect ? static_cast<ID3DXEffect*>(effectD3dxHandle(effect)) : 0;
    if (!d3dx)
    {
        if (effect)
            effectRelease(static_cast<Effect*>(effect));
        return false;
    }

    const double dt = secondsSinceLast(s);
    const bool snap = dt > kSnapSeconds || s.live < 0;
    const float speed = s.exposureSpeed;
    s.lastDt = static_cast<float>(dt);
    s.lastSnap = snap;

    Passes p;
    p.effect = effect;
    p.d3dx = d3dx;
    p.constants = d3dx->GetParameterBySemantic(0, "RaHdrPost");
    p.source = d3dx->GetParameterBySemantic(0, "RaHdrSource");
    p.history = d3dx->GetParameterBySemantic(0, "RaHdrHistory");
    p.c[1][0] = snap ? 1.0f : static_cast<float>(1.0 - std::exp(-dt * kRateLight * speed));
    p.c[1][1] = snap ? 1.0f : static_cast<float>(1.0 - std::exp(-dt * kRateDark * speed));
    p.c[1][2] = s.exposureBias;
    p.c[1][3] = snap ? 1.0f : 0.0f;
    p.c[2][0] = s.exposureMin;
    p.c[2][1] = s.exposureMax;
    p.c[2][2] = kKey;
    p.c[2][3] = kEvLow;
    p.c[3][0] = kEvRange;
    p.c[3][1] = kLowPercentile;
    p.c[3][2] = kHighPercentile;
    p.c[3][3] = kLuminanceCap;

    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    IDirect3DSurface9* savedDepth = stateManagerGetDepthStencilSurface();
    if (savedTarget)
        savedTarget->AddRef();
    if (savedDepth)
        savedDepth->AddRef();
    D3DVIEWPORT9 savedViewport;
    device()->GetViewport(&savedViewport);
    stateManagerPush();
    stateManagerSetDepthStencilSurface(0);

    // Down: all six levels
    bool ok = p.draw("RaBloomDownKaris", sceneTexture, scene->width, scene->height, s.level[0]);
    for (unsigned int i = 1; ok && i < kLevels; ++i)
        ok = p.draw("RaBloomDown", s.level[i - 1], s.levelW[i - 1], s.levelH[i - 1], s.level[i]);

    // Metering and adaptation, before the up chain adds into the meter's level
    bool metered = false;
    if (ok)
    {
        const int next = s.live == 0 ? 1 : 0;
        if (p.history)
            d3dx->SetTexture(p.history, s.state[1 - next]);   // unwritten before the first: snapped
        metered = p.draw("RaLumMeter", s.level[kMeterLevel], s.levelW[kMeterLevel], s.levelH[kMeterLevel], s.meter)
               && p.draw("RaLumHistogramRows", s.meter, kMeterW, kMeterH, s.rows)
               && p.draw("RaLumHistogram", s.rows, kGroups, 64, s.histogram)
               && p.draw("RaExposureAdapt", s.histogram, kGroups, 1, s.state[next]);
        if (p.history)
            d3dx->SetTexture(p.history, 0);
        if (metered)
            s.live = next;
    }
    s.stateLiveForParticles = metered;

    // Up: each level's tent added into the one above
    bool bloomed = false;
    if (ok)
    {
        bloomed = true;
        for (unsigned int i = kLevels - 1; bloomed && i > 0; --i)
            bloomed = p.draw("RaBloomUp", s.level[i], s.levelW[i], s.levelH[i], s.level[i - 1]);
    }
    if (p.source)
        d3dx->SetTexture(p.source, 0);

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
    ++s.frames;
    return ok && bloomed && metered;
}

} // anonymous namespace

void rendererHdrPost(EngineTexture* scene)
{
    State& s = st();
    s.thermal = thermalView();
    s.ran = !s.thermal && runPasses(scene);
    if (s.thermal)
        s.stateLiveForParticles = false;

    // The grade's parameters (FSQuadDrawer.fx, RaGrade)
    Effect* const fsq = geom::fsQuadDrawerEffect();
    ID3DXEffect* const d3dx = fsq ? static_cast<ID3DXEffect*>(effectD3dxHandle(fsq)) : 0;
    if (!d3dx)
        return;
    const bool autoExposure = s.ran && s.live >= 0;
    const bool bloom = s.ran;
    float g[2][4] = {
        { autoExposure ? 1.0f : 0.0f, bloom ? s.bloomStrength : 0.0f,
          s.thermal ? 0.0f : 1.0f, static_cast<float>(s.frames % 64) },
        { bloom ? 1.0f / s.levelW[0] : 0.0f, bloom ? 1.0f / s.levelH[0] : 0.0f,
          s.thermal ? 0.0f : s.exposureBias, 1.0f / kLevels },
    };
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaGrade"))
        d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(g), 2);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaGradeExposure"))
        d3dx->SetTexture(h, autoExposure ? s.state[s.live] : 0);
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaGradeBloom"))
        d3dx->SetTexture(h, bloom ? s.level[0] : 0);
    // The ceiling's adapted luminance: below it the exposure sits on exposureMax (the night)
    rendererGradeBind(d3dx, s.thermal, std::log2(kKey) + s.exposureBias - s.exposureMax);
}

void rendererHdrPostBindParticles(void* effect)
{
    State& s = st();
    ID3DXEffect* const d3dx = effect ? static_cast<ID3DXEffect*>(effectD3dxHandle(effect)) : 0;
    if (!d3dx)
        return;
    const bool autoExposure = s.stateLiveForParticles && s.live >= 0;
    const float c[4] = { autoExposure ? 1.0f : 0.0f, s.thermal ? 0.0f : s.exposureBias, 0.0f, 0.0f };
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaExposureParticle"))
        d3dx->SetVector(h, reinterpret_cast<const D3DXVECTOR4*>(c));
    if (const D3DXHANDLE h = d3dx->GetParameterBySemantic(0, "RaExposureState"))
        d3dx->SetTexture(h, autoExposure ? s.state[s.live] : 0);
}

bool rendererBloomReplacesGlow()
{
    return !thermalView();
}

void rendererHdrPostRelease()
{
    releaseAll();
}

float& rendererBloomStrength() { return st().bloomStrength; }
float& rendererExposureBias() { return st().exposureBias; }
float& rendererExposureMin() { return st().exposureMin; }
float& rendererExposureMax() { return st().exposureMax; }
float& rendererExposureSpeed() { return st().exposureSpeed; }

HdrPostInfo rendererHdrPostInfo()
{
    const State& s = st();
    HdrPostInfo i;
    i.ran = s.ran;
    i.thermal = s.thermal;
    i.bloomStrength = s.bloomStrength;
    i.exposureBias = s.exposureBias;
    i.exposureMin = s.exposureMin;
    i.exposureMax = s.exposureMax;
    i.exposureSpeed = s.exposureSpeed;
    i.key = kKey;
    i.evLow = kEvLow;
    i.evRange = kEvRange;
    i.lowPercentile = kLowPercentile;
    i.highPercentile = kHighPercentile;
    i.rateLight = kRateLight;
    i.rateDark = kRateDark;
    i.frames = s.frames;
    i.lastDt = s.lastDt;
    i.lastSnap = s.lastSnap;
    i.sceneW = s.w;
    i.sceneH = s.h;
    i.levels = kLevels;
    i.state = s.live >= 0 ? s.state[s.live] : 0;
    i.histogram = s.histogram;
    i.meter = s.meter;
    i.bloomTop = s.level[0];
    return i;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

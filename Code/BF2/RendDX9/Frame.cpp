// [RENDER] The frame module (Frame.h): the scene's pass sequence and the render targets it
// draws through - the HDR scene colour, the depth prepass and its linear depth, the resolve and
// depth rebuild before the world transparents. Not part of retail BF2. The order is described in
// docs/lanes/render.md (Frame). The pass sequence keeps retail's PE annotations where
// it still follows the shipped renderFrame (0x1002F7E0).
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "Frame.h"
#include "GpuTimers.h"                    // [DEV] BF2_GPU_TIMERS
#include "ToolsSurvey.h"                  // [TOOLS] tools.survey
#include "PostProductionFrameWork.h"
#include "QueueProfiler.h"
#include "ShaderParameter.h"
#include "Texture.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Mat4.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/HemiMapManager/HemiMapManager.h"
#include "../Geom/HemiMapManager/HemiMapDependencies.h"
#include "../Geom/Overgrowth/OverGrowth.h"
#include "../Geom/UndergrowthSystem.h"
#include "../Geom/WeatherManager.h"
#include "../Geom/Rain/RainManager.h"
#include "../Geom/ParticleSystem/ParticleSystemManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/TerrainAtlas.h"
#include "../Geom/SkyDome.h"
#include "../Geom/ObjectDrawer.h"
#include "../Geom/DecalSystem/DecalManager.h"
#include "../Geom/RoadEditableRenderer.h"
#include "../GeomMisc/FSQuadDrawer.h"     // textureHalfTexelSize

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dice {
namespace hfe {

namespace geom {
extern LightManager* g_LightManager;
extern IBase* g_SkyDome;
extern IBase* g_ParticleSystemManager;
extern IBase* g_DecalManager;
extern IBase* g_UndergrowthSystem;
extern IBase* g_RoadCompiledRenderer;
extern IBase* g_RoadEditableRenderer;
extern IBase* g_Terrain;
const float* roadCompiledRendererFadeOut();
void* roadCompiledRendererListener();
}

namespace dx9rend {

// BF2Render.cpp
extern void* g_rendererBase;
extern void* g_raShaderConstants;
extern void* g_renderTargetSystem;
extern void* g_OverGrowth;
extern IBase* g_PostProductionFrameWorkOwner;
extern double g_waterClock;
bool rendererBeginScene(const unsigned int* clearRgb, bool doClear);
bool rendererIsExpansionPack();
IRenderView* rendererGetRenderView();
IRenderView* rendererGetRenderViewDup();
IDirect3DSurface9* rendererSceneDepthStencil();     // +0x770, the scene's own depth-stencil
bool rendererShouldToggleTransparencyAA();
void rendererNoteOnce(unsigned int bit, const char* what);
void raShaderConstantsSetSunColor(const Vec3& colour);   // sub_100387E0
// StateManager.cpp
void flushRenderTargetsSeam();
void stateManagerSetRenderStatePersistent(unsigned int state, unsigned int value);
void stateManagerSetTransparencyAA(bool enabled);
// RenderQueues.cpp
void renderQueueFlush(unsigned int slotByteOffset, int flushArg);
bool renderQueueHasEntries(unsigned int slotByteOffset);
void renderQueuesDepthPrepass(unsigned int key);
unsigned int raHashString(const std::string& s);
// PostProductionFrameWork.cpp
bool postProductionFrameWorkIsActive();

// [RENDER] HDR scene colour. The scene stays in A16B16G16R16F until the
// frame grade (FSQuadDrawer pass 0) tonemaps it into the 8-bit back buffer. Without multisampling
// the scene target +0x788 itself is float. With multisampling retail draws the scene straight into
// the back buffer and resolves it into +0x788; here a multisampled float surface stands in for the
// back buffer from postProductionPrepare until the resolve in postProductionFinish, through the
// target selector's null-texture fallback (lightManagerSelectRenderTarget).
// [RENDER] Milestone 8: rendererSceneResolveForTransparents resolves the scene before the world
// transparents; from then until rendererHdrEndScene the scene is the resolved single-sample
// target +0x788 (g_resolvedSceneSurface), and the resolve in postProductionFinish is skipped.
namespace {
IDirect3DSurface9* g_hdrSceneSurface = 0;
bool g_hdrSceneActive = false;
IDirect3DSurface9* g_resolvedSceneSurface = 0;   // +0x788 level 0, referenced, while resolved

void endResolvedScene()
{
    if (g_resolvedSceneSurface)
        g_resolvedSceneSurface->Release();
    g_resolvedSceneSurface = 0;
}
}

IDirect3DSurface9* rendererSceneColorSurface()
{
    if (g_hdrSceneActive && g_resolvedSceneSurface)
        return g_resolvedSceneSurface;
    if (g_hdrSceneActive && g_hdrSceneSurface)
        return g_hdrSceneSurface;
    return rendererBackBufferSurface();                                // +0x76C
}

bool rendererSceneResolved()
{
    return g_hdrSceneActive && g_resolvedSceneSurface != 0;
}

void rendererHdrBeginScene()
{
    endResolvedScene();
    if (!rendererPostMultisampleActive())
        return;
    IDirect3DSurface9* const backBuffer = rendererBackBufferSurface();
    if (!backBuffer)
        return;
    if (!g_hdrSceneSurface)
    {
        D3DSURFACE_DESC desc;
        if (backBuffer->GetDesc(&desc) < 0)
            return;
        const HRESULT hr = static_cast<RendDX9Base*>(g_rendererBase)->device->CreateRenderTarget(
            desc.Width, desc.Height, D3DFMT_A16B16G16R16F, desc.MultiSampleType,
            desc.MultiSampleQuality, FALSE, &g_hdrSceneSurface, 0);
        std::fprintf(stderr, "[HDR] multisampled scene surface %ux%u x%d: hr=0x%08lX\n",
                     desc.Width, desc.Height, static_cast<int>(desc.MultiSampleType),
                     static_cast<unsigned long>(hr));
        if (hr < 0)
        {
            g_hdrSceneSurface = 0;
            return;
        }
    }
    g_hdrSceneActive = true;
}

// Ends the scene and, when the float surface was the current target, selects the back buffer:
// FSQuadDrawer restores the target that was current before its blit, and the HUD draws into
// whatever is current after the composite, so the float surface must not be current by then.
void rendererHdrEndScene()
{
    if (!g_hdrSceneActive)
        return;
    IDirect3DSurface9* const current = stateManagerGetRenderTarget(0);
    const bool sceneCurrent = current == g_hdrSceneSurface
        || (current && current == g_resolvedSceneSurface);
    g_hdrSceneActive = false;
    endResolvedScene();
    if (sceneCurrent)
        geom::lightManagerSetRenderTargetTexture(0, 0);       // null: the back buffer
}

void rendererHdrReleaseSurface()
{
    g_hdrSceneActive = false;
    endResolvedScene();
    if (g_hdrSceneSurface)
        g_hdrSceneSurface->Release();
    g_hdrSceneSurface = 0;
}

// [RENDER] Depth prepass, see RendDX9Base.h. The prepass draws into the scene depth; its
// linear depth goes to an R32F surface of the scene depth's size and sample count, resolved into the
// single-sample R32F texture bound to s15 (without multisampling it draws into the texture
// directly). Both are created on first use and released with the other render-path resources
// (device reset, resolution change). The resolve averages an edge pixel's samples into a depth no
// surface has; soft particles fade against that average.
namespace {
IDirect3DTexture9* g_linearDepthTexture = 0;
IDirect3DSurface9* g_linearDepthSamples = 0;   // multisampled, or null when the scene is not
const DWORD kLinearDepthSampler = 15;
float g_sunCascadeConstants[kSunCascadeVec4s * 4] = {};   // RASM for this frame's world view
float g_aerialConstants[4] = {};    // RaAerial for this frame's world view
float g_sunFactor[4] = {};          // RaSunFactor for this frame, every pass
const float kNoSunCascades[kSunCascadeVec4s * 4] = {};
float g_cloudShadowConstants[kCloudShadowVec4s * 4] = {};   // RaCloudShadow for this frame's world view
float g_sunStaticConstants[kSunStaticVec4s * 4] = {};       // RaSunStatic for this frame (milestone 14)
bool g_sunCascadesLive = false;     // RASM is published: the world pass or its transparents draw
bool g_softParticleDepth = false;   // s15 holds this frame's prepass depth and the world pass draws
// Milestone 8, transparent depth rebuild.
IDirect3DSurface9* g_rebuiltDepth = 0;   // single sample, the scene depth's format and size
bool g_linearDepthFrame = false;    // this frame's prepass drew; water and first person add to it
bool g_transparentDepth = false;    // the rebuilt depth is bound for the world transparents
float g_worldDepthProjection[2] = { 0.0f, 0.0f };   // the world view's P22, P32
float g_softParticleDistance = 1.5f;   // m, renderer.softParticleDistance

void bindSampler(DWORD stage, IDirect3DTexture9* texture, D3DTEXTUREFILTERTYPE filter)
{
    IDirect3DDevice9* const device = static_cast<RendDX9Base*>(g_rendererBase)->device;
    device->SetTexture(stage, texture);
    device->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    device->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

// RASM and RaAerial for this frame's world pass, or zero (nothing covered: every receiver keeps its
// baked sunlight; retail fog), to the pixel registers and for the particle effects.
// The frame's values stay in g_sunCascadeConstants and g_aerialConstants while zeros are published,
// so the world transparents get them back after the first-person passes.
void publishSunCascadeConstants(bool zero)
{
    g_sunCascadesLive = !zero;
    if (!g_rendererBase)
        return;
    stateManagerSetSunCascadeConstants(zero ? kNoSunCascades : g_sunCascadeConstants);
    stateManagerSetAerial(zero ? kNoSunCascades : g_aerialConstants);
    stateManagerSetCloudShadow(zero ? kNoSunCascades : g_cloudShadowConstants);
    stateManagerSetSunStatic(zero ? kNoSunCascades : g_sunStaticConstants);
    stateManagerSetSunFactor(g_sunFactor);   // the sun is the same in every pass
}

// The linear depth targets for the scene depth `depth`: same size, same sample count.
bool ensureLinearDepthTargets(IDirect3DSurface9* depth)
{
    D3DSURFACE_DESC desc;
    if (!depth || depth->GetDesc(&desc) < 0)
        return false;
    const bool multisampled = desc.MultiSampleType != D3DMULTISAMPLE_NONE;
    if (g_linearDepthTexture)
    {
        D3DSURFACE_DESC have;
        bool match = g_linearDepthTexture->GetLevelDesc(0, &have) >= 0
            && have.Width == desc.Width && have.Height == desc.Height
            && multisampled == (g_linearDepthSamples != 0);
        if (match && g_linearDepthSamples)
            match = g_linearDepthSamples->GetDesc(&have) >= 0
                && have.MultiSampleType == desc.MultiSampleType
                && have.MultiSampleQuality == desc.MultiSampleQuality;
        if (match)
            return true;
        rendererDepthPrepassRelease();
    }
    IDirect3DDevice9* const device = static_cast<RendDX9Base*>(g_rendererBase)->device;
    HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET,
                                       D3DFMT_R32F, D3DPOOL_DEFAULT, &g_linearDepthTexture, 0);
    if (hr >= 0 && multisampled)
        hr = device->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_R32F,
                                        desc.MultiSampleType, desc.MultiSampleQuality, FALSE,
                                        &g_linearDepthSamples, 0);
    std::fprintf(stderr, "[DEPTHPREPASS] R32F linear depth %ux%u x%d: hr=0x%08lX\n",
                 desc.Width, desc.Height, static_cast<int>(desc.MultiSampleType),
                 static_cast<unsigned long>(hr));
    if (hr < 0)
    {
        rendererDepthPrepassRelease();
        return false;
    }
    return true;
}
}

void* effectD3dxHandle(void* effect);   // Effect.cpp

// [RENDER] Particle lighting (milestone 7). The particle renderers set their own effect parameters, so
// RaShader::begin never reaches them; each renderer calls this as it binds its effect. The level's
// one sun as every lit surface gets it (Lightmanager.staticSunColor, staticSkyColor as authored),
// the direction toward the sun, the frame's RASM (zero outside the main world pass, so first
// person and other views keep the baked hemi map sun visibility), the cascade atlas and the sky's
// irradiance (RaSkyShVertex, the frame's RaSkySH: vertex shaders cannot read the pixel registers).
void rendererBindParticleLighting(void* effect)
{
    Effect* const fx = static_cast<Effect*>(effect);
    if (!fx || !geom::g_LightManager)
        return;
    const geom::LightManager* const lights = geom::g_LightManager;
    const Vec3 sunDir = lights->getSunDirection();
    const float length = ::sqrtf(sunDir.x * sunDir.x + sunDir.y * sunDir.y + sunDir.z * sunDir.z);
    const float scale = length > 0.0f ? -1.0f / length : 0.0f;
    const Vec3 towardSun = { sunDir.x * scale, sunDir.y * scale, sunDir.z * scale };
    static const std::string kSun("RaSunColor"), kSky("RaSkyColor"), kDir("RaSunDirection"),
        kCascades("RASM");
    effectGetShaderParameterBorrowedTyped(fx, kSun)->setValue(&lights->getStaticSunColor(), 0);
    effectGetShaderParameterBorrowedTyped(fx, kSky)->setValue(&lights->getStaticSkyColor(), 0);
    effectGetShaderParameterBorrowedTyped(fx, kDir)->setValue(&towardSun, 0);
    effectGetShaderParameterBorrowedTyped(fx, kCascades)->setValue(
        g_sunCascadesLive ? g_sunCascadeConstants : kNoSunCascades, kSunCascadeVec4s);
    static const std::string kCloudShadow("RaCloudShadow");
    effectGetShaderParameterBorrowedTyped(fx, kCloudShadow)->setValue(
        g_sunCascadesLive ? g_cloudShadowConstants : kNoSunCascades, kCloudShadowVec4s);
    static const std::string kSunFactor("RaSunFactorVertex");
    effectGetShaderParameterBorrowedTyped(fx, kSunFactor)->setValue(g_sunFactor, 0);
    static const std::string kSkySh("RaSkyShVertex");
    effectGetShaderParameterBorrowedTyped(fx, kSkySh)->setValue(rendererSkyShFrame(), kSkyShVec4s);
    // The atlas as a texture parameter: D3DX binds it to the vertex sampler it assigned. Set on
    // the D3DX effect directly, since the parameter path takes engine texture wrappers.
    if (ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect)))
        if (const D3DXHANDLE atlas = d3dx->GetParameterBySemantic(0, "RaSunCascadeAtlas"))
            d3dx->SetTexture(atlas, rendererSunShadowAtlas());
    if (ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effectD3dxHandle(effect)))
        if (const D3DXHANDLE map = d3dx->GetParameterBySemantic(0, "RaCloudShadowMap"))
            d3dx->SetTexture(map, rendererCloudShadowMap());

    // Soft particles: 1 / fade distance while the main world pass draws over this frame's prepass
    // depth (s15), and 1 / its size; 0 elsewhere, so first person and other views keep hard edges.
    float soft[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    D3DSURFACE_DESC depth;
    if (g_softParticleDepth && g_linearDepthTexture
        && g_linearDepthTexture->GetLevelDesc(0, &depth) >= 0)
    {
        soft[0] = 1.0f / g_softParticleDistance;
        soft[2] = 1.0f / static_cast<float>(depth.Width);
        soft[3] = 1.0f / static_cast<float>(depth.Height);
    }
    static const std::string kSoft("RaSoftParticles");
    effectGetShaderParameterBorrowedTyped(fx, kSoft)->setValue(soft, 0);
    rendererHdrPostBindParticles(effect);   // emissives undo the exposure (milestone 13)
}

float& softParticleDistance()
{
    return g_softParticleDistance;
}

IDirect3DTexture9* rendererLinearDepthTexture()
{
    return g_linearDepthTexture;
}

void rendererDepthPrepass(bool drawTerrain, float time)
{
    std::memset(g_sunCascadeConstants, 0, sizeof(g_sunCascadeConstants));
    publishSunCascadeConstants(true);
    g_softParticleDepth = false;
    g_linearDepthFrame = false;
    g_transparentDepth = false;
    IDirect3DSurface9* sceneDepth = stateManagerGetDepthStencilSurface();
    if (!sceneDepth)
        sceneDepth = rendererDepthStencilSurface();
    if (!ensureLinearDepthTargets(sceneDepth))
        return;

    // RASM: the cascade rows from the cascade pass, then the camera rows, the render view's camera
    // transform (right, up, forward, position) over the projection scales, so position
    // + clip.w * forward + clip.x * right' + clip.y * up' is the world point of a clip position.
    rendererSunShadowsRender(time, g_sunCascadeConstants);
    rendererSunStaticFrame(g_sunStaticConstants);   // [RENDER] milestone 14: the sun past the cascades
    rendererAtmosphereFrame();   // [RENDER] milestone 11: its tables and atlas
    rendererCloudsFrame();       // [RENDER] milestone 12: the clouds' volumes, over frames
    rendererWaterFrame();        // [RENDER] milestone 16: the waves
    rendererCloudShadowConstants(reinterpret_cast<float(*)[4]>(g_cloudShadowConstants));
    rendererAtmosphereDrawAtlas(reinterpret_cast<const float(*)[4]>(g_cloudShadowConstants));
    rendererAtmosphereAerialConstants(g_aerialConstants);
    rendererAtmosphereSunFactor(g_sunFactor);
    stateManagerSetSunFactor(g_sunFactor);
    {
        IRenderView* const view = rendererGetRenderView();
        const float* const c = view->getViewMatrixPtr()->m;
        const float* const p = view->getProjectionMatrixPtr()->m;
        const float sx = 1.0f / p[0];
        const float sy = 1.0f / p[5];
        g_worldDepthProjection[0] = p[10];
        g_worldDepthProjection[1] = p[14];
        const float camera[16] = {
            c[12], c[13], c[14], 1.0f,
            c[8], c[9], c[10], 0.0f,
            c[0] * sx, c[1] * sx, c[2] * sx, 0.0f,
            c[4] * sy, c[5] * sy, c[6] * sy, 0.0f };
        std::memcpy(g_sunCascadeConstants, camera, sizeof(camera));
    }

    IDirect3DDevice9* const device = static_cast<RendDX9Base*>(g_rendererBase)->device;
    device->SetTexture(kLinearDepthSampler, 0);       // never sampled while it is the target

    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    if (!savedTarget)
        savedTarget = rendererSceneColorSurface();
    if (savedTarget)
        savedTarget->AddRef();

    // The scene depth stays bound: it already holds the depth-only queue and terrain's first pass
    // (Terrain::drawMain), and the prepass adds every prepassed entry to it.
    gpuTimerMark("depth prepass");   // [DEV] BF2_GPU_TIMERS
    IDirect3DSurface9* resolved = 0;
    g_linearDepthTexture->GetSurfaceLevel(0, &resolved);
    IDirect3DSurface9* const target = g_linearDepthSamples ? g_linearDepthSamples : resolved;
    stateManagerSetRenderTarget(0, target);
    stateManagerSetDepthStencilSurface(sceneDepth);
    stateManagerClear(D3DCLEAR_TARGET, 0, 1.0f, 0);

    // Terrain draws its depth after the queues below.
    static const unsigned int key = raHashString(std::string("RaDepthPrepass"));
    // RaAerial for the prepass too (RASM stays zero): the view-distance cut (contract 9) runs in the
    // main pixel shader the prepass calls, so both must cut the same pixels.
    stateManagerSetAerial(g_aerialConstants);
    // Alpha-to-coverage: alpha-tested draws turn their alpha into a sample mask here, and their
    // lit passes test EQUAL against it with no alpha test of their own (stateManagerMapDepthState).
    // The vendor state (DXVK: NVIDIA's ATOC) acts only while a draw has its alpha test on, so
    // opaque draws in the bracket are unaffected. Terrain below draws outside it.
    const bool coverage = rendererShouldToggleTransparencyAA();
    if (coverage)
        stateManagerSetTransparencyAA(true);
    renderQueuesDepthPrepass(key);
    if (coverage)
        stateManagerSetTransparencyAA(false);

    // Terrain's depth and linear depth like any prepassed entry, last, so the depth test rejects what buildings and props in front of it already cover.
    if (drawTerrain && geom::g_Terrain)
    {
        gpuTimerMark("terrain depth");   // [DEV] BF2_GPU_TIMERS
        static_cast<geom::Terrain*>(geom::g_Terrain)->drawDepthPrepass();
        gpuTimerMark("depth prepass");   // [DEV] BF2_GPU_TIMERS
    }

    stateManagerSetRenderTarget(0, savedTarget);
    flushRenderTargetsSeam();
    if (g_linearDepthSamples && resolved)
        static_cast<RendDX9Base*>(g_rendererBase)->device->StretchRect(
            g_linearDepthSamples, 0, resolved, 0, D3DTEXF_NONE);
    if (resolved)
        resolved->Release();
    if (savedTarget)
        savedTarget->Release();

    bindSampler(kLinearDepthSampler, g_linearDepthTexture, D3DTEXF_POINT);
    publishSunCascadeConstants(false);
    g_softParticleDepth = true;
    g_linearDepthFrame = true;
}

void rendererDepthPrepassEndWorldPass()
{
    publishSunCascadeConstants(true);
    g_softParticleDepth = false;
}

namespace {
// Selects the linear depth samples (or the texture itself, unbound from s15, without
// multisampling) as render target 0 over the current scene depth; returns the previous target,
// referenced, for endLinearDepthTarget.
IDirect3DSurface9* beginLinearDepthTarget(IDirect3DSurface9*& resolved)
{
    IDirect3DSurface9* saved = stateManagerGetRenderTarget(0);
    if (!saved)
        saved = rendererSceneColorSurface();
    if (saved)
        saved->AddRef();
    static_cast<RendDX9Base*>(g_rendererBase)->device->SetTexture(kLinearDepthSampler, 0);
    resolved = 0;
    g_linearDepthTexture->GetSurfaceLevel(0, &resolved);
    stateManagerSetRenderTarget(0, g_linearDepthSamples ? g_linearDepthSamples : resolved);
    return saved;
}

void endLinearDepthTarget(IDirect3DSurface9* saved, IDirect3DSurface9* resolved)
{
    stateManagerSetRenderTarget(0, saved);
    flushRenderTargetsSeam();
    if (resolved)
        resolved->Release();
    if (saved)
        saved->Release();
    bindSampler(kLinearDepthSampler, g_linearDepthTexture, D3DTEXF_POINT);
}

// The single-sample depth the transparents test against after the resolve: the scene depth's
// format at the linear depth's size.
bool ensureRebuiltDepth()
{
    if (g_rebuiltDepth)
        return true;
    IDirect3DSurface9* const sceneDepth = rendererSceneDepthStencil();   // +0x770
    D3DSURFACE_DESC desc;
    if (!sceneDepth || sceneDepth->GetDesc(&desc) < 0)
        return false;
    const HRESULT hr = static_cast<RendDX9Base*>(g_rendererBase)->device->CreateDepthStencilSurface(
        desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rebuiltDepth, 0);
    std::fprintf(stderr, "[DEPTHREBUILD] single-sample depth %ux%u fmt %u: hr=0x%08lX\n",
                 desc.Width, desc.Height, static_cast<unsigned int>(desc.Format),
                 static_cast<unsigned long>(hr));
    if (hr < 0)
        g_rebuiltDepth = 0;
    return hr >= 0;
}

unsigned int kDepthPrepassKey()
{
    static const unsigned int key = raHashString(std::string("RaDepthPrepass"));
    return key;
}
}

void rendererLinearDepthWater()
{
    if (!g_linearDepthFrame)
        return;
    static const unsigned int kWater[] = { 0x28 };                          // RSMTWater
    IDirect3DSurface9* resolved;
    IDirect3DSurface9* const saved = beginLinearDepthTarget(resolved);
    renderQueuesDepthPass(kDepthPrepassKey(), kWater, 1, false);
    endLinearDepthTarget(saved, resolved);
}

// [RENDER] Milestone 11 task 12: the scene copy water reflects (see RendDX9Base.h).
namespace {
IDirect3DTexture9* g_waterSceneTexture = 0;
const float kNoWaterSsr[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

void releaseWaterScene()
{
    if (g_waterSceneTexture && g_rendererBase)
        static_cast<RendDX9Base*>(g_rendererBase)->device->SetTexture(kWaterSceneSampler, 0);
    if (g_waterSceneTexture)
        g_waterSceneTexture->Release();
    g_waterSceneTexture = 0;
}

// The copy's texture at the scene colour's size and format (single sample), with a full mip chain:
// rough water reads its reflections blurred (render milestone 16, task 3).
bool ensureWaterScene(const D3DSURFACE_DESC& scene)
{
    if (g_waterSceneTexture)
    {
        D3DSURFACE_DESC have;
        if (g_waterSceneTexture->GetLevelDesc(0, &have) >= 0 && have.Width == scene.Width
            && have.Height == scene.Height && have.Format == scene.Format)
            return true;
        releaseWaterScene();
    }
    const HRESULT hr = static_cast<RendDX9Base*>(g_rendererBase)->device->CreateTexture(
        scene.Width, scene.Height, 0, D3DUSAGE_RENDERTARGET, scene.Format, D3DPOOL_DEFAULT,
        &g_waterSceneTexture, 0);
    std::fprintf(stderr, "[WATERSSR] scene copy %ux%u fmt %u: hr=0x%08lX\n", scene.Width,
                 scene.Height, static_cast<unsigned int>(scene.Format),
                 static_cast<unsigned long>(hr));
    if (hr < 0)
        g_waterSceneTexture = 0;
    return hr >= 0;
}
}

void rendererWaterSceneBegin()
{
    if (!g_rendererBase)
        return;
    float constants[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (g_linearDepthFrame && !cameraIsUnderWater() && renderQueueHasEntries(0x28))
    {
        IDirect3DSurface9* const scene = rendererSceneColorSurface();
        D3DSURFACE_DESC desc;
        if (scene && scene->GetDesc(&desc) >= 0 && ensureWaterScene(desc))
        {
            flushRenderTargetsSeam();
            IDirect3DSurface9* copy = 0;
            g_waterSceneTexture->GetSurfaceLevel(0, &copy);
            if (copy && static_cast<RendDX9Base*>(g_rendererBase)->device->StretchRect(
                    scene, 0, copy, 0, D3DTEXF_NONE) >= 0)
            {
                // The mips, each a 2 x 2 box of the one above
                IDirect3DDevice9* const device = static_cast<RendDX9Base*>(g_rendererBase)->device;
                const DWORD levels = g_waterSceneTexture->GetLevelCount();
                for (DWORD l = 1; l < levels; ++l)
                {
                    IDirect3DSurface9* src = 0;
                    IDirect3DSurface9* dst = 0;
                    g_waterSceneTexture->GetSurfaceLevel(l - 1, &src);
                    g_waterSceneTexture->GetSurfaceLevel(l, &dst);
                    if (src && dst)
                        device->StretchRect(src, 0, dst, 0, D3DTEXF_LINEAR);
                    if (src)
                        src->Release();
                    if (dst)
                        dst->Release();
                }
                constants[0] = 1.0f;
                constants[1] = 1.0f / static_cast<float>(desc.Width);
                constants[2] = 1.0f / static_cast<float>(desc.Height);
            }
            if (copy)
                copy->Release();
        }
    }
    if (constants[0] > 0.0f)
    {
        bindSampler(kWaterSceneSampler, g_waterSceneTexture, D3DTEXF_LINEAR);
        static_cast<RendDX9Base*>(g_rendererBase)->device->SetSamplerState(kWaterSceneSampler,
            D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    }
    if (rendererWaterBindWaves())   // milestone 16: the waves (contract 17)
        constants[3] = 1.0f;
    stateManagerSetWaterSsr(constants);
    rendererSunStaticBind(3);   // milestone 14: water reads the sun's static layers on s3 (contract 36)
}

void rendererWaterSceneEnd()
{
    if (g_rendererBase)
    {
        stateManagerSetWaterSsr(kNoWaterSsr);
        rendererWaterUnbindWaves();
        static_cast<RendDX9Base*>(g_rendererBase)->device->SetTexture(3, 0);
        rendererSunStaticBind(kWaterSceneSampler);   // s12 is the sun's static layers again
    }
}

void rendererFirstPersonPrepass()
{
    if (!g_linearDepthFrame)
        return;
    static const unsigned int kFirstPerson[] = { 0x0C, 0x4C, 0x1C, 0x5C };  // RSMT*1Person, Aux*Person
    IDirect3DSurface9* resolved;
    IDirect3DSurface9* const saved = beginLinearDepthTarget(resolved);
    const bool coverage = rendererShouldToggleTransparencyAA();
    if (coverage)
        stateManagerSetTransparencyAA(true);
    renderQueuesDepthPass(kDepthPrepassKey(), kFirstPerson, 4, true);
    if (coverage)
        stateManagerSetTransparencyAA(false);
    endLinearDepthTarget(saved, resolved);
}

void rendererSceneResolveForTransparents()
{
    if (!g_linearDepthFrame)
        return;
    IDirect3DDevice9* const device = static_cast<RendDX9Base*>(g_rendererBase)->device;
    flushRenderTargetsSeam();
    IDirect3DSurface9* resolved = 0;
    g_linearDepthTexture->GetSurfaceLevel(0, &resolved);
    if (g_linearDepthSamples && resolved)
        device->StretchRect(g_linearDepthSamples, 0, resolved, 0, D3DTEXF_NONE);
    D3DSURFACE_DESC size = {};
    if (resolved)
    {
        resolved->GetDesc(&size);
        resolved->Release();
    }
    bindSampler(kLinearDepthSampler, g_linearDepthTexture, D3DTEXF_POINT);

    // The multisampled float scene resolves now; the transparents draw into the resolved target
    // over the rebuilt single-sample depth. Otherwise the rebuild overwrites the bound scene depth.
    if (g_hdrSceneActive && g_hdrSceneSurface && ensureRebuiltDepth())
    {
        rendererPostResolveScene();
        g_resolvedSceneSurface = textureGetLevelSurface(rendererPostSceneTexture(), 0, 0x7FFFFFFFu);
        stateManagerSetRenderTarget(0, g_resolvedSceneSurface);
        stateManagerSetDepthStencilSurface(g_rebuiltDepth);
        g_transparentDepth = true;
    }
    gpuTimerMark("depth rebuild");                    // [DEV] BF2_GPU_TIMERS
    const float constants[4] = {
        g_worldDepthProjection[0], g_worldDepthProjection[1],
        size.Width ? 1.0f / static_cast<float>(size.Width) : 0.0f,
        size.Height ? 1.0f / static_cast<float>(size.Height) : 0.0f };
    if (!postProductionDrawDepthRebuild(constants))
    {
        static bool reported = false;
        if (!reported)
            std::fprintf(stderr, "[DEPTHREBUILD] PostProduction has no RaDepthRebuild technique\n");
        reported = true;
    }
    publishSunCascadeConstants(false);
    g_softParticleDepth = true;
}

void rendererSceneTransparentsEnd()
{
    publishSunCascadeConstants(true);
    g_softParticleDepth = false;
    g_linearDepthFrame = false;
    if (g_transparentDepth)
    {
        // Flushed to the device now: the overlay passes save and restore the depth-stencil surface
        // straight from the device, and would carry the rebuilt one into the next frame.
        g_transparentDepth = false;
        stateManagerSetDepthStencilSurface(rendererSceneDepthStencil());   // +0x770
        flushRenderTargetsSeam();
    }
}

void rendererDepthPrepassRelease()
{
    if (g_linearDepthTexture && g_rendererBase)
        static_cast<RendDX9Base*>(g_rendererBase)->device->SetTexture(kLinearDepthSampler, 0);
    if (g_linearDepthTexture)
        g_linearDepthTexture->Release();
    g_linearDepthTexture = 0;
    if (g_linearDepthSamples)
        g_linearDepthSamples->Release();
    g_linearDepthSamples = 0;
    if (g_rebuiltDepth)
        g_rebuiltDepth->Release();
    g_rebuiltDepth = 0;
    releaseWaterScene();                             // [RENDER] milestone 11 task 12
    g_transparentDepth = false;
    g_linearDepthFrame = false;
}

IDirect3DSurface9* rendererDepthStencilSurface()
{
    if (g_transparentDepth && g_rebuiltDepth)
        return g_rebuiltDepth;          // [RENDER] milestone 8: the world transparents' rebuilt depth
    return rendererSceneDepthStencil();                              // +0x770
}

namespace {

// Post-production runs while its switch is on, its owner exists and, where the renderer asks for
// it, the framework reports itself active (retail tests all three at each of its three calls).
// sub_1004F1C0, the active test, reads the owner object's flags (+0x200, +0x24, +0x164, +0x27,
// +0x220, +0xFA, then the float +0xE0) through its owner TU, which knows the layout; a null owner
// answers false. The rebuilt owner is zeroed, so every term is false - the at-rest state of a
// real one.
bool postProductionOn(const FrameInputs& in)
{
    return in.postProduction && g_PostProductionFrameWorkOwner
        && (!in.postProductionNeedsActive || postProductionFrameWorkIsActive());
}

// sub_1002A560, retail Renderer::roadQueuePass. Updates road constants and flushes the road queue with depth bias.
// Requires a road renderer and terrain. Adds frame time again after the main accumulation.

void frameRoadPass(const FrameInputs& in)
{
    if (!geom::g_RoadCompiledRenderer || !geom::g_Terrain)
        return;

    IRenderView* const view = rendererGetRenderViewDup();                                   // +0x50
    // The Ra constants block by retail offset (raShaderConstantsAt maps it on x64, issue #7).
    *reinterpret_cast<float*>(raShaderConstantsAt(0x94)) += static_cast<float>(in.frameTime);
    g_waterClock += static_cast<float>(in.frameTime);

    {
        const float* const m = view->getViewMatrixPtr()->m;                                  // +0x64
        float* const matrix = reinterpret_cast<float*>(raShaderConstantsAt(0x1C));
        for (int i = 0; i < 16; ++i)
            matrix[i] = m[i];
        float* const camera = reinterpret_cast<float*>(raShaderConstantsAt(0x60));
        camera[0] = matrix[12];
        camera[1] = matrix[13];
        camera[2] = matrix[14];
    }

    Vec3 sun;
    if (geom::g_Terrain)
        static_cast<geom::Terrain*>(geom::g_Terrain)->getSunColorRaw(&sun);                 // +0x78
    else
    {
        sun.x = 1.0f;
        sun.y = 1.0f;
        sun.z = 1.0f;
    }
    float* const sunColor = reinterpret_cast<float*>(raShaderConstantsAt(0x11C));
    sunColor[0] = sun.x;
    sunColor[1] = sun.y;
    sunColor[2] = sun.z;

    const float* const fade = geom::roadCompiledRendererFadeOut();
    float* const roadFadeOut = reinterpret_cast<float*>(raShaderConstantsAt(0x12C));
    roadFadeOut[0] = fade[0];
    roadFadeOut[1] = fade[1];

    postProductionUpdateFog(underwaterDepth());

    stateManagerPush();
    // [RENDER] No depth bias: road shaders pull themselves toward the camera (docs/lanes/render.md#reversed-z).
    renderQueueFlush(0x24, 1);                                                               // RSMTRoad
    stateManagerPop();
}

} // anonymous namespace

// The scene of the shipped renderFrame (0x1002F7E0), from the post-production prepare through the
// post-production finish, with the passes this lane added.
bool frameRender(const FrameInputs& in, IDirect3DSurface9*& overlayDepthStencil)
{
    overlayDepthStencil = 0;
    if (postProductionOn(in))
    {
        rendererHdrBeginScene();                                     // [RENDER] HDR
        postProductionPrepare();
    }

    if (!rendererBeginScene(0, false))
        return false;

    // PE 0x1002FE52 passes XpLightManager.cpp's coordinator at 0x10259B20, not the
    // LightManager singleton.  The old guard/frontier conflated those two objects.
    geom::prepareLightFrame(in.time);
    // PE 0x1002FE5F: the mode selector's first-frame step (sub_1004DF10).
    postProductionModeSelectorFrameStart(g_renderTargetSystem);

    // [RENDER] Retail rendered the LightManager's shadow maps here (+0x19C): the sun maps
    // (replaced by the sun cascades) and the point/spot maps (deleted; docs/
    // renderer-pipeline.md, milestone 6). That pass ended by binding the scene's depth-stencil
    // (Renderer +0x770, reversed-Z) and colour target 0, which the frame clear and the
    // scene draw rely on; bind them here.
    geom::lightManagerSetDepthStencil(0);
    geom::lightManagerRestoreRendererTarget0();

    // sub_100057E0(7, 0, 1.0f, 0, 0, 0) - the frame clear: RT flush + Clear ALL, black.
    flushRenderTargetsSeam();
    {
        IDirect3DDevice9* dev = static_cast<RendDX9Base*>(g_rendererBase)->device;
        dev->Clear(0, 0, 7, 0, stateManagerClearDepth(1.0f), 0);   // [RENDER] reversed-Z aware
    }

    if (in.particles && geom::g_ParticleSystemManager)
    {
        geom::particleSystemManagerBeginFrame(
            geom::asParticleSystemManager(geom::g_ParticleSystemManager));
    }

    // sub_100383D0 - GlobalTime accumulate on the withheld constants block.
    if (g_raShaderConstants)
    {
        *reinterpret_cast<float*>(
            raShaderConstantsAt(148)) +=
            static_cast<float>(in.frameTime);
        g_waterClock += static_cast<float>(in.frameTime);
    }

    // The constants block is still unpublished in this
    // reconstruction, so retain its existing guard while using the exact LightManager
    // getters and destinations once it is present.
    if (g_raShaderConstants && geom::g_LightManager)
    {
        geom::LightManager* const lights = geom::g_LightManager;
        const Vec3& staticSky = lights->getStaticSkyColor();
        *reinterpret_cast<Vec3*>(raShaderConstantsAt(0x1F8)) = staticSky;

        const Vec3& treeSky = lights->getTreeSkyColor();
        Vec3& treeSkyConstant = *reinterpret_cast<Vec3*>(raShaderConstantsAt(0x154));
        treeSkyConstant.x = treeSky.x * 0.5f;
        treeSkyConstant.y = treeSky.y * 0.5f;
        treeSkyConstant.z = treeSky.z * 0.5f;

        *reinterpret_cast<Vec3*>(raShaderConstantsAt(0x214)) =
            lights->getStaticSpecularColor();
        *reinterpret_cast<Vec3*>(raShaderConstantsAt(0x230)) =
            lights->getSinglePointColor();
        *reinterpret_cast<Vec3*>(raShaderConstantsAt(0x24C)) =
            lights->getDynamicPointColor();
        *reinterpret_cast<unsigned int*>(raShaderConstantsAt(0x25C)) =
            rendererIsExpansionPack() ? 1u : 0u;
        *reinterpret_cast<float*>(raShaderConstantsAt(0x264)) =
            lights->getOccludedSkyNightIntensity();
    }

    // PE 0x1002FFED..0x10030006: with a terrain, its sun colour (+0x78, returned into the
    // argument slot) goes to the constants block's sun colour (sub_100387E0). The shipped
    // body tests only the terrain; the block guard follows the stores above.
    if (g_raShaderConstants && geom::g_Terrain)
    {
        Vec3 sun;
        static_cast<geom::Terrain*>(geom::g_Terrain)->getSunColorRaw(&sun);   // +0x78
        raShaderConstantsSetSunColor(sun);                                          // sub_100387E0
    }

    // the road fade pair into the constants block (sub_10038810), and the underwater fog
    // pair (sub_100F7B00 + sub_1004FDA0) - all constants-block writers, all guarded.
    if (g_raShaderConstants && geom::g_RoadCompiledRenderer)
    {
        const float* const fade = geom::roadCompiledRendererFadeOut();   // retail +0xB0
        raShaderConstantsSetRoadFade(fade[0], fade[1]);                  // sub_10038810
    }

    // the underwater fog pair, the camera's
    // depth below the water plane into PostProductionFrameWork::updateFog, which is what
    // writes the Ra FogRange/FogColor constants and D3DRS_FOGCOLOR every frame. The
    // shipped call is unguarded (ds:0x1023F7C0 straight into ecx); the guard follows this
    // function's convention for its singletons. [CHOSEN guard]
    if (g_raShaderConstants && g_PostProductionFrameWorkOwner)
        postProductionUpdateFog(underwaterDepth());

    if (in.world && in.objectDrawer)
    {
        typedef void (__thiscall* TimeFn)(void*, float);
        reinterpret_cast<TimeFn>((*reinterpret_cast<void***>(in.objectDrawer))[6])(
            in.objectDrawer, in.timeF);
    }

    renderQueueFlush(0x30, 1);                       // RSMTZOnly

    // Terrain is available after the level's terrain.create command.
    IBase* terrain = geom::g_Terrain;
    if (in.terrain && terrain)
    {
        gpuTimerMark("terrain begin");                                  // [DEV] BF2_GPU_TIMERS
        static_cast<geom::Terrain*>(terrain)->renderWorldPassBegin();   // +0xB8
        gpuTimerMark("pre-world");                                      // [DEV] BF2_GPU_TIMERS
        terrain = geom::g_Terrain;
    }

    // The world pass publishes the road shaders' terrain lightmap atlas grid and (GI colour, 1)
    // (TerrainLightAtlasGrid, TerrainLightAtlasGI; zero without an atlas), then the current base
    // Hemi map and its four constants. The shipped guard is only byte +0x69D; the old source also
    // tested dword 477 and thereby hid a real live path. [RENDER] Retail published the
    // accumulation target (+0x774) and its projection here; terrain and roads no longer use it.
    if (in.world)
    {
        float roadGrid[4] = {}, roadGI[4] = {};
        geom::terrainRoadLight(roadGrid, roadGI);
        raShaderConstantsSetTerrainLightAtlas(roadGrid, roadGI);

        geom::HemiMap* const hemiMap = geom::g_HemiMapManager->getBaseHemiMap();
        if (hemiMap)
        {
            raShaderConstantsSetHemiMapTexture(hemiMap->texture);
            const geom::HemiMapInfo& info =
                geom::g_HemiMapManager->getHemiMapInfo(hemiMap);
            raShaderConstantsSetHemiMapInfo(&info.positionX);
        }

        if (rendererShouldToggleTransparencyAA())
            stateManagerSetTransparencyAA(true);

        // fetch the view independently, then call +0x34.
        // Its genuine empty-path return is recovered in OverGrowth.cpp.
        if (in.overgrowth && g_OverGrowth)
        {
            IRenderView* const overGrowthView = rendererGetRenderView();
            static_cast<geom::OverGrowth*>(g_OverGrowth)->render(overGrowthView, in.timeF);
        }

        if (rendererShouldToggleTransparencyAA())
            stateManagerSetTransparencyAA(false);

        // [RENDER] Depth prepass: the queues are complete here (the
        // overgrowth enqueues above), and nothing has consumed them yet.
        rendererDepthPrepass(in.terrain && geom::g_Terrain, in.timeF);
        gpuTimerMark("world opaque");                // [DEV] BF2_GPU_TIMERS

        renderQueuesFlushWorldPass();

        if (rendererShouldToggleTransparencyAA())
            stateManagerSetTransparencyAA(true);

        // Draw grass inside the transparency-AA bracket.
        if (in.undergrowth && geom::g_UndergrowthSystem)
            static_cast<geom::UndergrowthSystem*>(geom::g_UndergrowthSystem)->render(in.timeF);

        if (rendererShouldToggleTransparencyAA())
            stateManagerSetTransparencyAA(false);

        terrain = geom::g_Terrain;
    }

    // PE 0x100303A7..0x100303BC mirrors the pre-detail Terrain arm at +0xBC.
    if (in.terrain && terrain)
    {
        gpuTimerMark("terrain");                                        // [DEV] BF2_GPU_TIMERS
        static_cast<geom::Terrain*>(terrain)->renderWorldPassEnd();     // +0xBC
    }

    gpuTimerMark("roads");                                              // [DEV] BF2_GPU_TIMERS
    if (in.roads)
        frameRoadPass(in);
    if (in.roads)
    {
        if (geom::g_RoadCompiledRenderer)
        {
            unsigned char* roadListener = static_cast<unsigned char*>(
                geom::roadCompiledRendererListener());                   // retail +0x24
            typedef void (__thiscall* Fn)(void*, int);
            reinterpret_cast<Fn>((*reinterpret_cast<void***>(roadListener))[3])(
                roadListener, 0);
        }
        if (geom::g_RoadEditableRenderer)
        {
            // IRoadEditableRenderer +0x18 drawAll, the slot is
            // the same in every flavour - IBase's four, then add / remove / drawAll.
            static_cast<geom::IRoadEditableRenderer*>(geom::g_RoadEditableRenderer)->drawAll();
        }
    }

    gpuTimerMark("sky");                                                // [DEV] BF2_GPU_TIMERS
    if (in.sky)
    {
        if (geom::g_SkyDome)
        {
            // The SkyDome's RenderPathListener subobject (retail +0x04), slot +0x0C.
            RenderPathListener* const skyListener =
                static_cast<geom::SkyDome*>(geom::g_SkyDome);
            skyListener->onRenderPathNotification(0);
            if (in.skyPass)
            {
                typedef void (__thiscall* SkyFn)(void*, int);
                reinterpret_cast<SkyFn>(
                    (*reinterpret_cast<void***>(geom::g_SkyDome))[42])(
                    geom::g_SkyDome, 0);
            }
        }
        else
            rendererNoteOnce(128u, "SkyDome (draw pass)");
    }

    // [RENDER] Milestone 12 task 4: the near clouds over the opaque scene and the sky, before the
    // world's transparents (which still draw over them).
    gpuTimerMark("clouds");                                             // [DEV] BF2_GPU_TIMERS
    if (in.sky && in.skyPass)
        rendererCloudsNear(!cameraIsUnderWater());

    gpuTimerMark("world transparent");               // [DEV] BF2_GPU_TIMERS

    if (in.world && cameraIsUnderWater())
    {
        renderQueueFlush(0x08, -1);                  // RSMTTransparency
        renderQueueFlush(0x48, -1);                  // RSMTAux0Transparency
        renderQueueFlush(0x18, -1);                  // RSMTDeltaTransparency
        renderQueueFlush(0x58, -1);                  // RSMTAux1Transparency
    }
    if (in.decals && geom::g_DecalManager)
    {
        // The manager's RenderPathListener subobject (retail +0x04), slot +0x0C.
        RenderPathListener* const decalListener = static_cast<geom::DecalManager*>(
            static_cast<geom::IDecalManager*>(geom::g_DecalManager));
        decalListener->onRenderPathNotification(0);
    }
    // PE 0x100304A5..0x100304C4: under water the additive queue is flushed here, before the
    // particles and the water; above water it waits for the transparency queues below.
    if (in.world && cameraIsUnderWater())
        renderQueueFlush(0x2C, 1);                   // RSMTAdditive (under water)
    if (in.particles && geom::g_ParticleSystemManager)
    {
        geom::asParticleSystemManager(geom::g_ParticleSystemManager)->onPathDraw(false, true);
    }
    // [RENDER] Milestone 8: water adds its linear depth before it draws, so the depth rebuilt
    // after the resolve keeps the transparents behind it.
    rendererLinearDepthWater();
    if (g_toolsSurveyOn)
        toolsSurveyWaterBegin();                     // [TOOLS] tools.survey: water's area
    rendererWaterSceneBegin();                       // [RENDER] milestone 11 task 12: water SSR
    renderQueueFlush(0x28, -1);                      // RSMTWater
    rendererWaterSceneEnd();
    if (g_toolsSurveyOn)
        toolsSurveyWaterEnd();

    // [RENDER] Milestone 8: the first-person passes draw here, into the 4x scene, where retail
    // drew them last (after the world transparents and the glow). They prepass like the
    // world (their own depth after the clear, and their linear depth over the world's),
    // so the world transparents, drawn after the resolve against the rebuilt depth, stay
    // behind them by the depth test instead of by draw order. The sun cascades describe
    // the world view only; the first-person passes keep their baked sunlight.
    rendererDepthPrepassEndWorldPass();
    gpuTimerMark("first person");                    // [DEV] BF2_GPU_TIMERS

    if (in.clearDepthBeforeFirstPerson)
    {
        // sub_100057E0(2, 0, 1.0f, 0, 0, 0) - the z-only clear before the first-person
        // passes.
        flushRenderTargetsSeam();
        IDirect3DDevice9* dev = static_cast<RendDX9Base*>(g_rendererBase)->device;
        dev->Clear(0, 0, 2, 0, stateManagerClearDepth(1.0f), 0);   // [RENDER] reversed-Z aware
    }

    rendererFirstPersonPrepass();
    renderQueueFlush(0x0C, in.clearDepthBeforeFirstPerson);              // RSMT1Person
    renderQueueFlush(0x4C, -1);                      // RSMTAux0Person
    renderQueueFlush(0x1C, in.clearDepthBeforeFirstPerson);              // RSMTDelta1Person
    renderQueueFlush(0x5C, -1);                      // RSMTAux1Person
    renderQueuesClearPrepassed();                    // [RENDER] the first-person prepass ends
    // PE 0x10030658..0x10030666: the first-person particles follow Aux1Person directly;
    // Aux1PersonTransparency is flushed once, last (0x100306BE).
    if (in.particles && in.clearDepthBeforeFirstPerson && geom::g_ParticleSystemManager)
    {
        geom::asParticleSystemManager(geom::g_ParticleSystemManager)->onPathDraw(true, false);
    }
    renderQueueFlush(0x10, in.clearDepthBeforeFirstPerson);              // RSMT1PersonTransparency
    renderQueueFlush(0x50, -1);                      // RSMTAux0PersonTransparency
    renderQueueFlush(0x20, in.clearDepthBeforeFirstPerson);              // RSMTDelta1PersonTransparency
    renderQueueFlush(0x60, -1);                      // RSMTAux1PersonTransparency

    // [RENDER] Milestone 8: resolve the scene and its linear depth, rebuild a single-sample
    // depth, and draw the world transparents single sample, with the world view's
    // cascades and soft particles back on.
    gpuTimerMark("resolve");                         // [DEV] BF2_GPU_TIMERS
    rendererSceneResolveForTransparents();
    gpuTimerMark("world transparent 1x");            // [DEV] BF2_GPU_TIMERS
    if (in.world)
    {
        if (!cameraIsUnderWater())
        {
            renderQueueFlush(0x08, -1);
            renderQueueFlush(0x48, -1);
            renderQueueFlush(0x18, -1);
            renderQueueFlush(0x58, -1);
        }
        if (!cameraIsUnderWater())
            renderQueueFlush(0x2C, 1);               // RSMTAdditive
    }
    if (in.particles && geom::g_ParticleSystemManager)
    {
        geom::asParticleSystemManager(geom::g_ParticleSystemManager)->onPathDraw(false, false);
    }
    if (in.weatherManager && in.particles)
        geom::asWeatherManager(in.weatherManager)->render();                      // +0x1C
    if (in.rainManager && in.particles)
        geom::asRainManager(in.rainManager)->render();                            // +0x1C
    if (in.sky && in.skyPass && geom::g_SkyDome)
    {
        typedef void (__thiscall* SkyFn)(void*, int);
        reinterpret_cast<SkyFn>((*reinterpret_cast<void***>(geom::g_SkyDome))[42])(
            geom::g_SkyDome, 1);
    }
    if (postProductionOn(in))
    {
        gpuTimerMark("post pass");                   // [DEV] BF2_GPU_TIMERS
        postProductionPass();
    }
    rendererSceneTransparentsEnd();                  // [RENDER] the scene depth again

    // PE 0x100306D2..0x1003070C: with a multisampled back buffer the overlay passes run
    // without multisampling and without a depth-stencil surface; the surface is put back
    // after the console draw (0x10030AE7..0x10030AFA), by the caller. The shipped body never
    // releases the reference GetDepthStencilSurface adds; the caller drops it after the restore.
    overlayDepthStencil = 0;
    if (rendererPostMultisampleActive())                                             // sub_1000A6B0
    {
        stateManagerSetRenderStatePersistent(161, 0);    // D3DRS_MULTISAMPLEANTIALIAS
        IDirect3DDevice9* const dev = static_cast<RendDX9Base*>(g_rendererBase)->device;
        dev->GetDepthStencilSurface(&overlayDepthStencil);                   // +0xA0
        dev->SetDepthStencilSurface(0);                                    // +0x9C
    }

    gpuTimerMark("post finish");                     // [DEV] BF2_GPU_TIMERS
    // PE 0x10030712..0x100307A0: the profiler is initialized on first use.
    if (postProductionOn(in))
    {
        static QueueProfiler profile(std::string("PostProduction"));
        profile.begin();
        postProductionFinish();
        QueueProfiler::end();
    }
    rendererHdrEndScene();          // [RENDER] HDR: the HUD and overlays go to the back buffer
    gpuTimerMark("hud");            // [DEV] BF2_GPU_TIMERS

    return true;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

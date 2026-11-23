// Terrain visibility, shader constants, diffuse passes and water rendering.
// The grid-water path is selected when ShaderVersion is zero, as in stock shaders.
// Ra water and spot-light profiling retain named boundaries. Older per-frame paths still omit
// their diagnostic-only profiling brackets; those omissions are documented at their callers.
#ifdef BF2_IMAGE_RENDDX9

#if defined(__APPLE__)
#include "../Platform/X87Compat.h" // [CHOSEN, not recovered]
#endif
#include "../../RendDX9Abi/PluginAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include <string>
#include "Terrain.h"
#include "QuadPatch.h"
#include "TerrainAtlas.h"
#include "FarGround.h"
#include "XpLightManager.h"
#include "StaticMesh.h"
#include "../Game/Objects/Visual/LightSource.h"   // the mask-light pass's lights
#include "../Math/Frustum.h"
#include "../RendDX9/RendDX9Math.h"       // the RendDX9.dll Math copies
#include "../Math/Mat4.h"
#include "../Physics/HeightmapCluster.h"
#include "../Game/Main/IVideoOptions.h"
#include "../RendDX9/RendDX9Base.h"
#include "../RendMisc/EnvMapSystem/EnvMapManager.h"
#include "../RendDX9/ShaderParameter.h"
#include "../RendDX9/Texture.h"
#include "../RendDX9/Ra/RaShader.h"
#include "../RendDX9/Ra/RaStreamDataEditable.h"
#include "../RendMisc/IRenderView.h"
#include "../RendMisc/IRenderer.h"      // getRenderStats (+0x6C)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <cstdint>
#include "../GeomMisc/FSQuadDrawer.h"   // textureHalfTexelSize, PE 0x100846A0

namespace dice {
namespace hfe {

namespace dx9rend {

extern void* g_rendererBase;       // RendDX9Init.cpp   - dword_1023B970 (the Renderer object)
extern void* g_shaderManager;      // ShaderManager.cpp - dword_1023B97C
extern void* g_raShaderConstants;  // RenderQueues.cpp  - dword_1023DDBC (the Ra constants block)
extern double g_waterClock;        // BF2Render.cpp     - [CHOSEN, not recovered] GlobalTime as a double

bool  effectHasTechnique(void* effect, const std::string& name);          // Effect.cpp,  sub_1001DC20
void  updateShaderDefines(void* shaderManager, void* base);                // ShaderManager.cpp, 0x10023730
void  renderQueueFlush(unsigned int slotByteOffset, int flushArg);         // RenderQueues.cpp
void  vertexBufferBindStream(void* wrapper, unsigned int stream);          // BufferManager.cpp, sub_100146C0
void  indexBufferBind(void* wrapper);
void  vertexDeclarationBind(void* wrapper);
void* indexBufferLock(void* wrapper, unsigned int offsetIndices, unsigned int countIndices,
                      unsigned int flags);
bool  indexBufferUnlock(void* wrapper);
void  rendererDrawIndexedPrimitive(unsigned int primCount, unsigned int type, int baseVertex,
                                   unsigned int minIndex, unsigned int numVertices,
                                   unsigned int startIndex);               // StateManager.cpp, sub_10005820

// [RENDER] renderer.hexTiling (render milestone 19): the detail layers hex-tiled (1) or tiled on a
// square grid as retail (0).
bool g_hexTiling = true;
bool& rendererHexTiling() { return g_hexTiling; }

} // namespace dx9rend

namespace geom {

extern LightManager* g_LightManager;                                       // ModuleGeom.cpp, dword_1024B914
void geometryRendererMatrixProduct(Mat4&, const Mat4&, const Mat4&);

namespace {

inline dx9rend::ShaderParameter* param(void* handle)
{
    return static_cast<dx9rend::ShaderParameter*>(handle);
}

// The Ra constants block (g_raShaderConstants) words, by their retail x86 byte offsets.
inline unsigned char* constantAt(unsigned int byteOffset)
{
    return dx9rend::raShaderConstantsAt(byteOffset);
}
inline float& constantF(unsigned int byteOffset)
{
    return *reinterpret_cast<float*>(constantAt(byteOffset));
}
inline void*& constantP(unsigned int byteOffset)
{
    return *reinterpret_cast<void**>(constantAt(byteOffset));
}

inline ::dice::hfe::world::IHeightmapCluster* heightmapClusterByName()
{
    IBase* const found = ::dice::hfe::classManager->getSingleton(
        ::renddx9abi::HostString("HeightmapCluster"));
    return static_cast< ::dice::hfe::world::IHeightmapCluster*>(static_cast<void*>(found));
}

// The .data constants the water arms read. byte_10239200: the morph limits are squared; the
// flt_10239208 / flt_10239204 pair is WaterScroll.zw; byte_1023920C gates the two cell tests.
const unsigned char kSquaredMorphLimits = 1;                               // byte_10239200
const float         kWaterScrollZ       = 0.3f;                            // flt_10239208
const float         kWaterScrollW       = 0.5f;                            // flt_10239204
const unsigned char kWaterCellTests     = 3;                               // byte_1023920C
// flt_101DDF30 (0x3E4CCCCD) - the 2D water's cycle: one pass over the frame list every
// waterAnimSpeed * 0.2 seconds of GlobalTime (updateWaterConstants, 0x100DBBB6).
const float         kWater2DCycleScale  = 0.2f;
// [CHOSEN, not recovered] The 3D water's wrap period in cycles (updateWaterConstants): about 7 to
// 28 hours for the stock waterAnimSpeed range of 50 to 200. Below it a float keeps the cycle to
// about 6e-5, a small fraction of a texel.
const double        kWater3DCyclePeriod = 1000.0;

// EnvMapManager - the singleton dword_10267A34, now published by RendModule. Its real
// level/frame lifecycle supplies the borrowed +64 texture used by the detail pass.

} // anonymous namespace

IBase* terrainEnvMapManager()
{
    return Rend::g_EnvMapManager;
}


// The shared helpers QuadPatch.cpp and SurroundingPatch.cpp reach (declared in Terrain.h).

unsigned int& terrainRendererStat(unsigned int byteOffset)
{
    // Renderer vslot +0x6C (getRenderStats, retail `return this + 264`), then the counter.
    unsigned char* const stats = static_cast<unsigned char*>(
        static_cast<dx9rend::IRenderer*>(dx9rend::g_rendererBase)->getRenderStats());
    return *reinterpret_cast<unsigned int*>(stats + byteOffset);
}


bool terrainFrustumIntersectsBox(const Frustum* frustum, const float* minMax)
{
    const float* plane = reinterpret_cast<const float*>(frustum);           // m_planes[0] at +0
    for (unsigned int i = 0; i < 6; ++i, plane += 4)
    {
        float x, y, z;
        switch (frustum->m_planeOutcodes[i])
        {
        case 0:  x = minMax[0]; y = minMax[1]; z = minMax[2]; break;
        case 1:  x = minMax[3]; y = minMax[1]; z = minMax[2]; break;
        case 2:  x = minMax[0]; y = minMax[4]; z = minMax[2]; break;
        case 3:  x = minMax[3]; y = minMax[4]; z = minMax[2]; break;
        case 4:  x = minMax[0]; y = minMax[1]; z = minMax[5]; break;
        case 5:  x = minMax[3]; y = minMax[1]; z = minMax[5]; break;
        case 6:  x = minMax[0]; y = minMax[4]; z = minMax[5]; break;
        case 7:  x = minMax[3]; y = minMax[4]; z = minMax[5]; break;
        default: x = 0.0f; y = 0.0f; z = 0.0f; break;
        }
        if (static_cast<double>(z) * plane[2] + static_cast<double>(x) * plane[0]
            + static_cast<double>(y) * plane[1] + plane[3] > 0.0)
            return false;
    }
    return true;
}

// Typed sky-light direction/color accessors preserve host interface semantics.
const float* terrainSkyLightDirection(world::LightSource* light)
{
    return &light->getDirection().x;                                               // +0x17C
}

const float* terrainSkyLightColor(world::LightSource* light)
{
    return &light->getColor().x;                                                   // +0x198
}


// The two arms Renderer::renderFrame calls, and update (slot +0x28).

// the culler's per-frame step, then the water constants.
void Terrain::update()
{
    m_culler->update();
    updateWaterConstants();                                               // vslot +0xE0
    terrainAtlasUpdate();                                                 // [RENDER] TerrainAtlas.h, once after a load
    if (terrainAtlases())
        farGroundUpdate(this);                                            // [RENDER] FarGround.h, once, after the atlases
}

// the first arm: push, update, the constants, the main draw, the water, pop.
void Terrain::renderWorldPassBegin()
{
    dx9rend::stateManagerPush();
    update();                                                             // vslot +0x28
    applyFrameConstants();
    if (m_drawMain)
        drawMain();
    if (m_flag35A)
        drawWater();                                                      // vslot +0xD8
    dx9rend::stateManagerPop();
}

// the closing arm: the diffuse passes, then the ring. [RENDER] The far ground in the ring's place
// (render milestone 17, FarGround.h); the ring wherever it does not draw (renderer.farGround).
void Terrain::renderWorldPassEnd()
{
    dx9rend::stateManagerPush();
    if (m_drawMain)
        drawFinal();
    if (m_flag359 && !farGroundDraw(this) && m_surroundingBuilt)
        drawSurrounding();
    dx9rend::stateManagerPop();
}


// (slot +0xE0) - the water quality latch, then the water constants of the Ra block.

void Terrain::updateWaterConstants()
{
    // byte_10258F70 - g_waterQualityLatched, Terrain.cpp. The latch: a quality that differs from
    // the applied one sets it, unless the quality is still -1 (never read), which clears it and
    // reads video option 0 now; an equal pair re-reads only while the latch is clear.
    bool read;
    if (m_waterQuality == m_waterQualityApplied)
        read = terrainWaterQualityLatched() == 0;
    else
    {
        terrainWaterQualityLatched() = 1;
        read = (m_waterQuality == -1);
        if (read)
            terrainWaterQualityLatched() = 0;
    }
    if (read)
        m_waterQuality = dx9rend::rendererGetVideoOptions()->getQuality(0);   // base +0x74, then +0x04
    if (m_waterQuality != m_waterQualityApplied)
    {
        setupWater();                                                     // vslot +0xD4
        m_waterQualityApplied = m_waterQuality;
    }

    // Both arms compute in float: the x87 runs under the single-precision control word D3D9 sets for
    // a device created without D3DCREATE_FPU_PRESERVE (behaviour flags 0x50 - NameTagRenderer.cpp
    // records the same), so every product, quotient and difference below is rounded to float, and
    // is written as one (RW-240: this arm was computed in double, which moves
    // the phase and the frame pair at every cycle boundary). The truncations are 32-bit FISTPs
    // under a chop control word - a C cast toward zero, 0x80000000 when out of range, as this
    // build's cvttss2si.
    if (m_water3D)
    {
        // Retail reads GlobalTime before the vslot +0x90 call (0x100DBB52) and writes
        // WaterCycleTime = frac(GlobalTime / waterAnimSpeed) (`fdivr`, `fld st0`, `fistp`, `fild`,
        // `fsubr`). The 3D water shaders offset their texture by WaterScroll * WaterCycleTime and
        // WaterCycleTime * 10 + 0.7 * u + 1.13 * v, so the wrap makes the surface jump every
        // waterAnimSpeed / 2 seconds.
        // [CHOSEN, not recovered] B2BF/OpenBF2 issue 69: the cycle runs on unwrapped, at the same
        // rate, from the double-precision copy of GlobalTime. It wraps only every
        // kWater3DCyclePeriod cycles to keep the float constant precise in long sessions; at that
        // period every texture offset jumps by a whole number for scroll values with one decimal,
        // which covers the stock levels. The 2D arm below stays retail: it uses the constant as a
        // 0..1 frame blend.
        const double cycle = dx9rend::g_waterClock / getWaterAnimSpeed();             // vslot +0x90
        constantF(412) = static_cast<float>(std::fmod(cycle, kWater3DCyclePeriod));  // WaterCycleTime
    }
    else
    {
        // Select and blend two water frames from time/(animationSpeed * 0.2).
        // setupWater must supply a nonempty frame list; sampling has no null/empty guards.
        const float globalTime = constantF(148);                                      // GlobalTime
        const float cycleLength = getWaterAnimSpeed() * kWater2DCycleScale;           // vslot +0x90, fmul
        const float cycle = globalTime / cycleLength;                                 // fdivr
#if defined(__APPLE__)
        m_waterTextureList->phase = cycle - static_cast<float>(bf2x87::toIntTruncate(cycle));
#else
        m_waterTextureList->phase = cycle - static_cast<float>(static_cast<int>(cycle));
#endif
        dx9rend::raShaderConstantsSetEngineTexture(0x1A8, m_waterTextureList->currentFrame());  // sub_10014EC0, WaterMapFrame0
        dx9rend::raShaderConstantsSetEngineTexture(0x1B0, m_waterTextureList->nextFrame());     // sub_10014EF0, WaterMapFrame1
        constantF(412) = m_waterTextureList->frameBlend();                                      // sub_10014F30, WaterCycleTime
    }

    Vec2 scroll;
    constantF(392) = getWaterScroll(&scroll)->x;                          // vslot +0x88, called twice as shipped
    constantF(396) = getWaterScroll(&scroll)->y;
    constantF(400) = kWaterScrollZ;
    constantF(404) = kWaterScrollW;

    ::dice::hfe::world::IHeightmapCluster* const cluster = heightmapClusterByName();
    const float seaLevel = cluster->getSeaWaterLevel();                   // vslot +0x60, three calls as shipped
    constantF(444) = cluster->getSeaWaterLevel();                         // WaterHeight
    {
        dx9rend::IRenderView* const view = dx9rend::rendererGetRenderView();       // base +0x48
        const float* const m = view->getViewMatrixPtr()->m;               // +0x64
        constantF(472) = m[13] - cluster->getSeaWaterLevel();             // CameraToWater
    }
    constantF(480) = 1.0f;                                                // WaterClipDir
    {
        dx9rend::IRenderView* const view = dx9rend::rendererGetRenderView();
        dx9rend::Ra::RaObjectParameters& op =
            *reinterpret_cast<dx9rend::Ra::RaObjectParameters*>(m_objectParameters);
        ::memcpy(op.viewProjection.value, view->getViewProjectionMatrixPtr()->m, 64);   // +0x84, sub_10030E80
        float* const world = op.world.value;
        world[0] = 1.0f; world[1] = 0.0f; world[2] = 0.0f; world[3] = 0.0f; world[4] = 0.0f;
        world[5] = 1.0f; world[6] = 0.0f; world[7] = 0.0f; world[8] = 0.0f; world[9] = 0.0f;
        world[10] = 1.0f; world[11] = 0.0f; world[12] = 0.0f; world[13] = 0.0f; world[14] = 0.0f;
        world[15] = 1.0f;
        world[12] = 0.0f;
        world[13] = seaLevel;
        world[14] = 0.0f;
        op.tail220 = 0;
        // [RENDER] Retail copied the first directional shadow record's matrices here; water
        // samples the sun cascades instead.
    }
    setWaterRecordConstants();
}

// sub_1018F8D0 on the seven-float record: WaterColor, then transparency and reflectivity by the
// camera's side of the water plane.
void Terrain::setWaterRecordConstants()
{
    const bool under = dx9rend::cameraIsUnderWater();
    const float* const r = m_waterRecord;
    constantF(452) = r[0];
    constantF(456) = r[1];
    constantF(460) = r[2];
    constantF(464) = 1.0f;
    constantF(488) = r[3 + 2 * (under ? 1 : 0)];                          // WaterTransparency
    constantF(496) = r[4 + 2 * (under ? 1 : 0)];                          // WaterReflectivity
}


// sub_100DB980 / sub_100DB9A0 - renderer.waterColor's get and set arms (RendererConsole.cpp): the
// record's first three floats, unguarded (the record exists from the constructor on). Placement
// beside the other water-record users is [INFERRED] from the address order.
Vec3 Terrain::getWaterColor() const
{
    const float* const r = m_waterRecord;
    Vec3 colour;
    colour.x = r[0]; colour.y = r[1]; colour.z = r[2];
    return colour;
}

void Terrain::setWaterColor(Vec3 colour)
{
    float* const r = m_waterRecord;
    r[0] = colour.x; r[1] = colour.y; r[2] = colour.z;
}

// sub_100DB9E0 / 0x100DBA00 / 0x100DBA50 / 0x100DBA90 - the terrain console's waterTransparency
// and waterReflectivity arms: the above-water floats of the record (+0x0C / +0x10), unguarded.
float Terrain::getWaterTransparency() const { return m_waterRecord[3]; }
float Terrain::getWaterReflectivity() const { return m_waterRecord[4]; }
void  Terrain::setWaterTransparency(float value) { m_waterRecord[3] = value; }
void  Terrain::setWaterReflectivity(float value) { m_waterRecord[4] = value; }

// The unreachable water-record forwarders (no code or data references in 1.50). The record's
// colour accessors scale by 255 / 1/255 (sub_1018F940 / sub_1018F970); names are [INFERRED].

// PE 0x100DB9C0. Unreachable in 1.50: no code or data references.
Vec3* Terrain::getWaterColor255(Vec3* out)
{
    const float* const r = m_waterRecord;
    out->x = r[0] * 255.0f;
    out->y = r[1] * 255.0f;
    out->z = r[2] * 255.0f;
    return out;
}

// PE 0x100DBA20. Unreachable in 1.50: no code or data references.
void Terrain::setWaterColor255(Vec3 colour)
{
    float* const r = m_waterRecord;
    r[0] = colour.x * 0.003921569f;                                           // 0x3B808081
    r[1] = colour.y * 0.003921569f;
    r[2] = colour.z * 0.003921569f;
}

// PE 0x100DBA70. Unreachable in 1.50: no code or data references.
void Terrain::setWaterTransparencyBelow(float value)
{
    m_waterRecord[5] = value;
}

// PE 0x100DBAB0. Unreachable in 1.50: no code or data references.
void Terrain::setWaterReflectivityBelow(float value)
{
    m_waterRecord[6] = value;
}

// sub_100DB030 / sub_100DB000 - terrain.ambientColor: +0x2FC as stored; the set raises +0x41C.
Vec3* Terrain::getAmbientColor(Vec3* out) const
{
    *out = m_ambientColor;
    return out;
}

void Terrain::setAmbientColor(float r, float g, float b)
{
    m_ambientColor.x = r;
    m_ambientColor.y = g;
    m_ambientColor.z = b;
    m_lightingDirty = 1;
}

// sub_100311D0 / sub_10031200 - terrain.GIColorHigh / GIColorLow's set arms: the colour as given.
// [INFERRED] placement; the shipped bodies sit outside the terrain's address range.
void Terrain::setGIColorHigh(const Vec3& colour) { m_giColorHigh = colour; }
void Terrain::setGIColorLow(const Vec3& colour)  { m_giColorLow = colour; }


// the constants worker: matVIEWPROJ, the camera, the colours and fog, the water
// colour and maps, then - when the lighting is dirty - the morph limits and the tiling constants.

void Terrain::applyFrameConstants()
{
    dx9rend::Effect* const fx = static_cast<dx9rend::Effect*>(m_effect);
    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();        // base +0x50
    {
        // Fetch projection first, then multiply the inverse-view matrix by it.
        const Mat4* const proj  = view->getProjectionMatrixPtr();         // +0x6C
        const Mat4* const viewM = view->getInverseViewMatrixPtr();        // +0x68
        reinterpret_cast<Mat4*>(m_viewProj)->fullMult(*viewM, *proj);
    }
    param(m_hMatViewProj)->setValue(m_viewProj, 0);
    {
        const float* const camera = view->getViewMatrixPtr()->m + 12;     // +0x64 row 3
        const float* const packed = staticMeshPackVec4(camera, 1.0f);
        m_cameraPosition[1] = packed[1];
        m_cameraPosition[0] = packed[0];
        m_cameraPosition[2] = packed[2];
        m_cameraPosition[3] = packed[3];
    }
    if (!m_heightmapCluster)
        m_heightmapCluster = heightmapClusterByName();
    if (m_heightmapCluster)
    {
        const float cameraY = m_cameraPosition[1];
        const Vec3* const p = terrainOriginScratch(*reinterpret_cast<const Vec3*>(m_cameraPosition));
        const double h = m_heightmapCluster->getHeightInWorldCoords(*p);   // vslot +0x68
        const double d = fabs(static_cast<double>(cameraY) - h);
        m_cameraHeightAboveTerrain = static_cast<float>(d);
        m_cameraPosition[3] = static_cast<float>(d + 1.0);
    }
    param(m_hCameraPos)->setValue(m_cameraPosition, 0);
    {
        // [RENDER] Terrain is drawn at full detail (retail's LOD 0) with no geomorph. What remains
        // of retail's morph constants at LOD 0 is the MorphDelta.z term every vertex of 0x1A
        // content takes (the morphdeltaadder rows were all (0, 0, primary.y) at LOD 0): the
        // shader subtracts MorphDelta.z times this. Older content has no such term.
        const float scale = (m_rawHasVersion && m_rawVersion >= 0x1Au) ? m_primaryWorldScale.y : 0.0f;
        param(m_hHeightDeltaScale)->setFloat(scale);
    }
    param(m_hAmbientColor)->setValue(&m_ambientColor, 0);
    {
        Vec3 sun;
        getSunColorRaw(&sun);                            // [RENDER] the level's one sun
        param(m_hSunColor)->setValue(&sun, 0);
    }
    param(m_hGIColor)->setValue(&m_giColor, 0);
    {
        float single[3];
        single[0] = static_cast<float>(constantF(560) * 0.25);
        single[1] = static_cast<float>(constantF(564) * 0.25);
        single[2] = static_cast<float>(constantF(568) * 0.25);
        param(m_hSinglePointColor1X)->setValue(single, 0);
    }
    param(m_hDynamicPointColor)->setValue(constantAt(588), 0);
    param(m_hNightTimeMode)->setValue(constantAt(604), 0);
    param(m_hNightOccludedSkyIntensity)->setValue(constantAt(612), 0);
    param(m_hFogRange)->setValue(constantAt(168), 0);
    param(m_hFogColor)->setValue(constantAt(188), 0);
    {
        float range[4];                                                   // sub_100380E0: +208..+220
        range[0] = constantF(208); range[1] = constantF(212); range[2] = constantF(216); range[3] = constantF(220);
        param(m_hAmbientIlluminationRange)->setValue(range, 0);
        float colour[4];                                                  // sub_10038080: +228..+240
        colour[0] = constantF(228); colour[1] = constantF(232); colour[2] = constantF(236); colour[3] = constantF(240);
        param(m_hAmbientIlluminationColor)->setValue(colour, 0);
    }
    dx9rend::postProductionUpdateFog(dx9rend::underwaterDepth());         // sub_1004FDA0(sub_100F7B00())
    param(m_hCameraToWater)->setFloat(constantF(472));

    ::dice::hfe::world::IHeightmapCluster* const cluster = heightmapClusterByName();
    float seaLevel = cluster->getSeaWaterLevel();                         // +0x60
    {
        const float* const m = view->getViewMatrixPtr()->m;
        constantF(464) = static_cast<float>(
            (20.0 - m[13] + seaLevel) * 0.050000001 * m_waterAlphaHeightMul + m_waterAlphaAdd);   // WaterColor.w
    }
    {
        float rgb[3] = { constantF(452), constantF(456), constantF(460) };
        const float* const packed = staticMeshPackVec4(rgb, 1.0f);
        float value[4] = { packed[0], packed[1], packed[2], packed[3] };
        param(m_hTerrainWaterColor)->setValue(value, 0);
    }
    seaLevel = static_cast<float>(seaLevel * 0.33333334);
    param(m_hWaterHeight)->setValue(&seaLevel, 0);
    if (constantP(420))
        param(m_hCausticsMap)->setValue(constantP(420), 0);
    if (constantP(428))
        param(m_hCausticsMapFrame0)->setValue(constantP(428), 0);
    if (constantP(436))
        param(m_hCausticsMapFrame1)->setValue(constantP(436), 0);
    {
        float xyz[3] = { constantF(392), constantF(396), constantF(400) };
        const float* const packed = staticMeshPackVec4(xyz, 1.0f);
        float value[4] = { packed[0], packed[1], packed[2], packed[3] };
        param(m_hWaterScroll)->setValue(value, 0);
    }
    param(m_hWaterCycleTime)->setFloat(constantF(412));

    if (!m_lightingDirty)
        return;
    m_lightingDirty = 0;
    param(m_hTexLayer[4])->setValue(m_lowDetailTexture, 0);
    if (m_drawMain)
    {
        // [RENDER] Retail bound the accumulation target (+0x774) on TEXLAYER1 here with its screen
        // projection (TEXPROJSCALE/OFFSET); the one pass binds each patch's lightmap instead.
        {
            float scaleTransY[4] = { m_primaryWorldScale.y, 1.0f, 0.0f, 0.0f };
            param(m_hScaleTransY)->setValue(scaleTransY, 0);
        }
        float texScale[4];
        texScale[0] = static_cast<float>(255.0 / static_cast<double>(m_patchSize));
        texScale[1] = -0.0061514801f;
        texScale[2] = texScale[0];
        texScale[3] = 0.0f;
        param(m_hTexScale)->setValue(texScale, 0);
        const float farTop = m_farTopTilingHi;                            // [RENDER] one terrain quality
        {
            float farTiling[4] = { m_farSideTiling.x, m_farSideTiling.y, farTop, m_farYOffset };
            param(m_hFarTexTiling)->setValue(farTiling, 0);
        }
        {
            float yPlane[4] = { farTop * texScale[0], farTop * texScale[1], farTop * texScale[2], farTop * texScale[3] };
            param(m_hYPlaneTexScaleAndFarTile)->setValue(yPlane, 0);
        }
        {
            const double w = static_cast<double>(
                static_cast<dx9rend::EngineTexture*>(m_patches[0]->lightmap)->width);
            float colorLightTex[2] = { 0.0f, 0.0f };
            colorLightTex[0] = static_cast<float>((w - 1.0) / w);
            colorLightTex[1] = static_cast<float>(1.0 / (w + w));
            dx9rend::effectGetShaderParameterBorrowedTyped(fx, std::string("COLORLIGHTTEX"))->setValue(colorLightTex, 0);
        }
        if (m_patchCount * m_patchCount > 0)
        {
            int i = 0;
            bool found = true;
            while (!m_patches[i]->detailMap[0])
            {
                ++i;
                if (i >= m_patchCount * m_patchCount)
                {
                    found = false;
                    break;
                }
            }
            if (found)
            {
                const double w = static_cast<double>(
                    static_cast<dx9rend::EngineTexture*>(m_patches[i]->detailMap[0])->width);
                float detailTex[2] = { 0.0f, 0.0f };
                detailTex[0] = static_cast<float>((w - 1.0) / w);
                detailTex[1] = static_cast<float>(1.0 / (w + w));
                dx9rend::effectGetShaderParameterBorrowedTyped(fx, std::string("DETAILTEX"))->setValue(detailTex, 0);
            }
        }
    }
    if (m_flag359 && m_surroundingBuilt)
    {
        {
            float stScaleTransY[4] = { m_secondaryWorldScale.y, 1.0f, 0.0f, 0.0f };
            param(m_hStScaleTransY)->setValue(stScaleTransY, 0);
        }
        {
            float stTexScale[4];
            stTexScale[0] = static_cast<float>(1.0 / static_cast<double>(m_patchSize) / m_secondaryWorldScale.x);
            stTexScale[1] = static_cast<float>(m_secondaryWorldScale.y * 400.0 * 0.0000153787);
            stTexScale[2] = stTexScale[0];
            stTexScale[3] = 0.0f;
            param(m_hStTexScale)->setValue(stTexScale, 0);
        }
        {
            const double patches = static_cast<double>(m_patchCount);
            const double quarter = m_secondaryWorldScale.x * 0.25;
            float stFar[4];
            stFar[0] = static_cast<float>(patches * m_farSideTiling.x / quarter);
            stFar[1] = m_farSideTiling.y;
            stFar[2] = static_cast<float>(patches * m_farTopTilingHi / quarter);   // [RENDER] one terrain quality
            stFar[3] = m_farYOffset;
            param(m_hStFarTexTiling)->setValue(stFar, 0);
        }
        int s = 0;
        while (!m_surrounding[s])
        {
            ++s;
            if (s >= 8)
                return;
        }
        {
            const double w = static_cast<double>(
                static_cast<dx9rend::EngineTexture*>(m_surrounding[s]->colormap)->width);
            float value[2] = { 0.0f, 0.0f };
            value[0] = static_cast<float>((w - 1.0) / w);
            value[1] = static_cast<float>(1.0 / (w + w));
            dx9rend::effectGetShaderParameterBorrowedTyped(fx, std::string("STCOLORLIGHTTEX"))->setValue(value, 0);
        }
        {
            const double w = static_cast<double>(
                static_cast<dx9rend::EngineTexture*>(m_surrounding[s]->lowDetailmap)->width);
            float value[2] = { 0.0f, 0.0f };
            value[0] = static_cast<float>((w - 1.0) / w);
            value[1] = static_cast<float>(1.0 / (w + w));
            dx9rend::effectGetShaderParameterBorrowedTyped(fx, std::string("STLOWDETAILTEX"))->setValue(value, 0);
        }
    }
}

// PE 0x100DB2F0. The argument is not read.
void Terrain::drawDepthComplexity(int)
{
    update();
    Mat4 viewProjection = Mat4::getIdentity();
    dx9rend::IRenderView* const view =
        static_cast<dx9rend::IRenderer*>(dx9rend::g_rendererBase)->getRenderViewDup();
    const Mat4* const projection = view->getProjectionMatrixPtr();
    const Mat4* const inverseView = view->getInverseViewMatrixPtr();
    const float* const a = inverseView->m;
    const float* const b = projection->m;
    // PE 0x10077A30: each component has its own shipped association and store order.
    // [CHOSEN, not recovered] Native macOS retains the x87 intermediate width.
#if defined(__APPLE__)
    using DepthReal = bf2x87::Real;
#else
    using DepthReal = double;
#endif
    viewProjection.m[0] = static_cast<float>(((((DepthReal(a[0]) * DepthReal(b[0])) + (DepthReal(b[8]) * DepthReal(a[2]))) + (DepthReal(a[1]) * DepthReal(b[4]))) + (DepthReal(a[3]) * DepthReal(b[12]))));
    viewProjection.m[4] = static_cast<float>(((((DepthReal(a[5]) * DepthReal(b[4])) + (DepthReal(b[8]) * DepthReal(a[6]))) + (DepthReal(a[7]) * DepthReal(b[12]))) + (DepthReal(b[0]) * DepthReal(a[4]))));
    viewProjection.m[8] = static_cast<float>(((((DepthReal(a[11]) * DepthReal(b[12])) + (DepthReal(a[8]) * DepthReal(b[0]))) + (DepthReal(a[10]) * DepthReal(b[8]))) + (DepthReal(a[9]) * DepthReal(b[4]))));
    viewProjection.m[12] = static_cast<float>(((((DepthReal(b[8]) * DepthReal(a[14])) + (DepthReal(a[13]) * DepthReal(b[4]))) + (DepthReal(a[15]) * DepthReal(b[12]))) + (DepthReal(a[12]) * DepthReal(b[0]))));
    viewProjection.m[1] = static_cast<float>(((((DepthReal(b[13]) * DepthReal(a[3])) + (DepthReal(a[1]) * DepthReal(b[5]))) + (DepthReal(b[1]) * DepthReal(a[0]))) + (DepthReal(b[9]) * DepthReal(a[2]))));
    viewProjection.m[5] = static_cast<float>(((((DepthReal(a[5]) * DepthReal(b[5])) + (DepthReal(b[1]) * DepthReal(a[4]))) + (DepthReal(a[7]) * DepthReal(b[13]))) + (DepthReal(a[6]) * DepthReal(b[9]))));
    viewProjection.m[9] = static_cast<float>(((((DepthReal(b[5]) * DepthReal(a[9])) + (DepthReal(b[1]) * DepthReal(a[8]))) + (DepthReal(a[11]) * DepthReal(b[13]))) + (DepthReal(a[10]) * DepthReal(b[9]))));
    viewProjection.m[13] = static_cast<float>(((((DepthReal(b[1]) * DepthReal(a[12])) + (DepthReal(b[13]) * DepthReal(a[15]))) + (DepthReal(b[9]) * DepthReal(a[14]))) + (DepthReal(b[5]) * DepthReal(a[13]))));
    viewProjection.m[2] = static_cast<float>(((((DepthReal(b[14]) * DepthReal(a[3])) + (DepthReal(a[1]) * DepthReal(b[6]))) + (DepthReal(b[2]) * DepthReal(a[0]))) + (DepthReal(b[10]) * DepthReal(a[2]))));
    viewProjection.m[6] = static_cast<float>(((((DepthReal(a[5]) * DepthReal(b[6])) + (DepthReal(b[2]) * DepthReal(a[4]))) + (DepthReal(a[7]) * DepthReal(b[14]))) + (DepthReal(a[6]) * DepthReal(b[10]))));
    viewProjection.m[10] = static_cast<float>(((((DepthReal(b[6]) * DepthReal(a[9])) + (DepthReal(b[2]) * DepthReal(a[8]))) + (DepthReal(a[11]) * DepthReal(b[14]))) + (DepthReal(a[10]) * DepthReal(b[10]))));
    viewProjection.m[14] = static_cast<float>(((((DepthReal(b[2]) * DepthReal(a[12])) + (DepthReal(b[14]) * DepthReal(a[15]))) + (DepthReal(b[10]) * DepthReal(a[14]))) + (DepthReal(b[6]) * DepthReal(a[13]))));
    viewProjection.m[3] = static_cast<float>(((((DepthReal(a[0]) * DepthReal(b[3])) + (DepthReal(b[15]) * DepthReal(a[3]))) + (DepthReal(b[11]) * DepthReal(a[2]))) + (DepthReal(a[1]) * DepthReal(b[7]))));
    viewProjection.m[7] = static_cast<float>(((((DepthReal(b[11]) * DepthReal(a[6])) + (DepthReal(a[5]) * DepthReal(b[7]))) + (DepthReal(b[3]) * DepthReal(a[4]))) + (DepthReal(a[7]) * DepthReal(b[15]))));
    viewProjection.m[11] = static_cast<float>(((((DepthReal(a[8]) * DepthReal(b[3])) + (DepthReal(b[7]) * DepthReal(a[9]))) + (DepthReal(a[11]) * DepthReal(b[15]))) + (DepthReal(a[10]) * DepthReal(b[11]))));
    viewProjection.m[15] = static_cast<float>(((((DepthReal(b[11]) * DepthReal(a[14])) + (DepthReal(b[15]) * DepthReal(a[15]))) + (DepthReal(b[7]) * DepthReal(a[13]))) + (DepthReal(a[12]) * DepthReal(b[3]))));
    param(m_hMatViewProj)->setValue(&viewProjection, 0);
    float scaleTransY[4] = { m_primaryWorldScale.y, 1.0f, 0.0f, 0.0f };
    param(m_hScaleTransY)->setValue(scaleTransY, 0);
    if (m_compactVertices)
        dx9rend::vertexBufferBindStream(m_baseGridVertexBuffer, 0);
    dx9rend::indexBufferBind(m_lodIndexBuffer);
    dx9rend::vertexDeclarationBind(m_vertexDeclaration);
    const Mat4* const cameraMatrix = view->getViewMatrixPtr();
    float camera[3] = { cameraMatrix->m[12], cameraMatrix->m[13], cameraMatrix->m[14] };
    {
        const std::string name("CAMERAPOS");
        dx9rend::effectGetShaderParameterBorrowedTyped(
            static_cast<dx9rend::Effect*>(m_effect), name)->setValue(camera, 0);
    }
    dx9rend::stateManagerPush();
    dx9rend::effectSetTechniqueByName(m_effect, std::string("RPDirectX9DepthComplexity"));
    dx9rend::effectBegin(m_effect);
    dx9rend::effectBeginPass(m_effect, 0);
    std::vector<TerrainCullerNode*>* const list = m_culler->visiblePatches();
    TerrainCullerNode** it = list->data();
    // Shipped defect: the obsolete patch draw only emits an error and never advances it.
    while (it != list->data() + list->size())
        (*it)->patch->drawDepthComplexity(it, 0);
    m_culler->filterOccluded();

    dx9rend::effectBeginPass(m_effect, 1);
    it = list->data();
    while (it != list->data() + list->size())
    {
        QuadPatch* const patch = (*it)->patch;
        param(m_hTexLayer[0])->setValue(patch->colormap, 0);
        patch->drawDepthComplexity(it, 1);
    }
    if (m_compactVertices)
        dx9rend::vertexBufferBindStream(m_baseGridVertexBuffer, 0);
    dx9rend::indexBufferBind(m_lodIndexBuffer);
    dx9rend::vertexDeclarationBind(m_vertexDeclaration);
    dx9rend::effectBeginPass(m_effect, 2);
    it = list->data();
    while (it != list->data() + list->size())
    {
        QuadPatch* const patch = (*it)->patch;
        param(m_hTexLayer[0])->setValue(patch->lightmap, 0);
        patch->drawDepthComplexity(it, 2);
    }
    dx9rend::effectBeginPass(m_effect, 3);
    it = list->data();
    while (it != list->data() + list->size())
    {
        QuadPatch* const patch = (*it)->patch;
        param(m_hTexLayer[0])->setValue(patch->colormap, 0);
        param(m_hTexLayer[1])->setValue(patch->lightmap, 0);
        patch->drawDepthComplexity(it, 3);
    }
    dx9rend::effectEnd(m_effect);
    dx9rend::stateManagerPop();
}


// PE 0x100DB810. [INFERRED] Method and argument names. [RENDER] Nothing calls it; retail drew one
// Hi_Terrain pass over the visible or frustum-gathered nodes. The slot stays for the table layout.
void Terrain::drawTechniquePass(Frustum*, int, int)
{
}

// [DEV] terrain.survey: the last frame's point-light source (0 none, 1 the static frame lights, 2
// the visible mask lights), the lights it gathered and their nodes.
static int g_surveyLightPath = 0;
static unsigned int g_surveyLights = 0;
static unsigned int g_surveyLightNodes = 0;

// [RENDER] The point lights reaching each patch, gathered by drawMain through the culler's sphere
// gather and handed to RaTerrain as constants (RaTerrainLightPos/Col), the nearest to the patch
// centre when more than kPatchLights reach it.
namespace {

const int kPatchLights = 4;   // RA_TERRAIN_MAX_LIGHTS in the terrain shader

struct PatchLights
{
    QuadPatch* patch;
    int        count;
    float      distance[kPatchLights];     // squared, light to patch centre
    float      position[kPatchLights][4];  // xyz, 1 / range^2
    float      color[kPatchLights][4];     // authored colour
};

std::vector<PatchLights> g_patchLights;

void addPatchLight(QuadPatch* patch, const Vec3& position, float inverseRangeSquared, const Vec3& color)
{
    PatchLights* entry = 0;
    for (size_t i = 0; i != g_patchLights.size(); ++i)
        if (g_patchLights[i].patch == patch)
        {
            entry = &g_patchLights[i];
            break;
        }
    if (!entry)
    {
        PatchLights fresh = {};
        fresh.patch = patch;
        g_patchLights.push_back(fresh);
        entry = &g_patchLights.back();
    }
    const float distance = patch->getSquaredDistance(position);
    int slot = entry->count;
    if (slot == kPatchLights)
    {
        slot = 0;
        for (int i = 1; i < kPatchLights; ++i)
            if (entry->distance[i] > entry->distance[slot])
                slot = i;
        if (entry->distance[slot] <= distance)
            return;
    }
    else
        ++entry->count;
    entry->distance[slot] = distance;
    entry->position[slot][0] = position.x;
    entry->position[slot][1] = position.y;
    entry->position[slot][2] = position.z;
    entry->position[slot][3] = inverseRangeSquared;
    entry->color[slot][0] = color.x;
    entry->color[slot][1] = color.y;
    entry->color[slot][2] = color.z;
    entry->color[slot][3] = 0.0f;
}

// One light over the nodes a sphere gather returned (consecutive nodes share a patch).
void addGatheredLight(std::vector<TerrainCullerNode*>* gathered, const Vec3& position,
                      float inverseRangeSquared, const Vec3& color)
{
    QuadPatch* previous = 0;
    for (size_t i = 0; i != gathered->size(); ++i)
    {
        QuadPatch* const patch = (*gathered)[i]->patch;
        if (patch == previous)
            continue;
        previous = patch;
        addPatchLight(patch, position, inverseRangeSquared, color);
    }
}

const PatchLights* findPatchLights(const QuadPatch* patch)
{
    for (size_t i = 0; i != g_patchLights.size(); ++i)
        if (g_patchLights[i].patch == patch)
            return &g_patchLights[i];
    return 0;
}

} // anonymous namespace

// PE 0x100E0940. [RENDER] Terrain's depth is drawn by the depth prepass and its colour by
// drawFinal in one pass, so the main draw only flushes the aux queues and gathers the point lights
// for RaTerrain. Retail drew pass 0 (depth and the packed lightmap) into the accumulation target
// (+0x774), then the point-light passes (16, or 13 and 1) over it.
void Terrain::drawMain()
{
    m_shadowPatches.clear();                                              // sub_100E1C20(0, 0)
    dx9rend::renderQueueFlush(0x44u, 1);                                  // RSMTAux0..2 through their +0x08 slot
    dx9rend::renderQueueFlush(0x54u, 1);
    dx9rend::renderQueueFlush(0x64u, 1);
    g_surveyLightPath = 0;                                                // [DEV] terrain.survey
    g_patchLights.clear();
    g_surveyLights = 0;
    g_surveyLightNodes = 0;
    if (dx9rend::rendererGetHemiRenderMode() != 4)                        // sub_10009F00, base +0x20
        gatherPointLights();
}


// [RENDER] Depth prepass: the visible patches through the
// effect's RaDepthPrepass technique into the renderer's linear depth target. It uses the patch
// draw of drawMain's pass 0, so positions match the passes that follow.
// [RENDER] Sun shadow casters, see Terrain.h.
namespace {
struct TerrainCaster
{
    TerrainCullerNode* node;
    float              sphere[4];
};
std::vector<TerrainCaster> g_terrainCasters;

void gatherTerrainCasterNode(TerrainCullerNode* node, const Frustum& volume)
{
    TerrainAabb& box = node->bounds;
    if (box.dirty)
        box.refresh();
    if (!rendmath::frustumInside(volume, box.centre, box.radius))
        return;
    if (!node->fileByteEE)
    {
        if (node->child[0])
            for (int c = 0; c < 4; ++c)
                gatherTerrainCasterNode(node->child[c], volume);
        return;
    }
    if (!node->patch)
        return;
    TerrainCaster caster;
    caster.node = node;
    caster.sphere[0] = box.centre.x;
    caster.sphere[1] = box.centre.y;
    caster.sphere[2] = box.centre.z;
    caster.sphere[3] = box.radius;
    g_terrainCasters.push_back(caster);
}
}

void Terrain::gatherShadowCasters(const Frustum& volume)
{
    g_terrainCasters.clear();
    if (!m_drawMain || !m_culler || !m_culler->nodes || !m_culler->cameraPosition)
        return;
    gatherTerrainCasterNode(m_culler->nodes, volume);
}

void Terrain::drawShadowCasters(const float* viewProjection, const dx9rend::ShadowCascadeCull& cull)
{
    static const std::string technique("RaShadowDepth");
    if (g_terrainCasters.empty() || !m_effect || !dx9rend::effectHasTechnique(m_effect, technique))
        return;
    static dx9rend::Effect* parameterEffect = 0;
    static dx9rend::ShaderParameter* viewProjectionParameter = 0;
    dx9rend::Effect* const effect = static_cast<dx9rend::Effect*>(m_effect);
    if (parameterEffect != effect)
    {
        parameterEffect = effect;
        viewProjectionParameter =
            dx9rend::effectGetShaderParameterTyped(effect, std::string("RaShadowViewProj"));
    }
    if (!viewProjectionParameter)
        return;

    dx9rend::stateManagerPush();
    if (m_compactVertices)
        dx9rend::vertexBufferBindStream(m_baseGridVertexBuffer, 0);
    dx9rend::vertexDeclarationBind(m_vertexDeclaration);
    const std::string savedTechnique = dx9rend::effectCurrentTechniqueName(m_effect);
    dx9rend::effectSetTechniqueByName(m_effect, technique);
    dx9rend::effectBegin(m_effect);
    viewProjectionParameter->setValue(viewProjection, 0);
    dx9rend::indexBufferBind(m_lodIndexBuffer);
    dx9rend::effectBeginPass(m_effect, 0u);
    QuadPatch* bound = 0;
    for (size_t i = 0; i < g_terrainCasters.size(); ++i)
    {
        const TerrainCaster& caster = g_terrainCasters[i];
        if (!dx9rend::rendererShadowCascadeTouches(cull, caster.sphere))
            continue;
        QuadPatch* const patch = caster.node->patch;
        if (patch != bound)
        {
            patch->beginCasterDraw();
            bound = patch;
        }
        patch->drawCasterNode(caster.node);
    }
    dx9rend::effectEnd(m_effect);
    if (!savedTechnique.empty())
        dx9rend::effectSetTechniqueByName(m_effect, savedTechnique);
    dx9rend::stateManagerPop();
}

void Terrain::drawDepthPrepass()
{
    drawMainDepthPrepass();
    // The far ground after the map, where renderWorldPassEnd draws it.
    if (m_flag359 && m_effect)
    {
        dx9rend::stateManagerPush();
        farGroundDrawDepth(this);
        dx9rend::stateManagerPop();
    }
}

void Terrain::drawMainDepthPrepass()
{
    static const std::string technique("RaDepthPrepass");
    if (!m_drawMain || !m_effect
        || !dx9rend::effectHasTechnique(m_effect, technique))
        return;
    std::vector<TerrainCullerNode*>* const list = m_culler->visiblePatches();
    dx9rend::stateManagerPush();
    if (m_compactVertices)
        dx9rend::vertexBufferBindStream(m_baseGridVertexBuffer, 0);
    dx9rend::vertexDeclarationBind(m_vertexDeclaration);
    dx9rend::effectSetTechniqueByName(m_effect, technique);
    dx9rend::effectBegin(m_effect);
    dx9rend::indexBufferBind(m_lodIndexBuffer);
    dx9rend::effectBeginPass(m_effect, 0u);
    TerrainCullerNode** it = list->data();
    while (it != list->data() + list->size())
        (*it)->patch->draw(it);                                           // advances `it`
    dx9rend::effectEnd(m_effect);
    dx9rend::stateManagerPop();
}

namespace {

float terrainScaledInverseRangeSquared(float radius, float scale)
{
#if defined(__APPLE__)
    // [CHOSEN, not recovered] The scaled radius is not stored before squaring in the PE.
    const bf2x87::Real scaled = bf2x87::mul(bf2x87::wide(scale), bf2x87::wide(radius));
    return bf2x87::store(bf2x87::div(bf2x87::wide(1.0f), bf2x87::mul(scaled, scaled)));
#else
    const float scaled = scale * radius;
    return 1.0f / (scaled * scaled);
#endif
}

} // anonymous namespace

// [RENDER] The point lights reaching terrain, by the source retail's light passes drew from:
// with the effect's ShaderVersion 1 the frame's static point lights (PE 0x100DF990, pass 16),
// otherwise the visible lights of mask 2313 (PE 0x100DE5B0, passes 13 and 1). Attenuation uses
// dynamicLightRangeScale; the gather radius stays unscaled.
void Terrain::gatherPointLights()
{
    const float rangeScale = g_LightManager->getDynamicLightRangeScale();
    if (m_effect && *dx9rend::effectShaderVersion(m_effect) == 1)
    {
        const std::vector<XpLight*>& lights = xpStaticFrameLights();
        for (XpLight* const* light = lights.data(); light != lights.data() + lights.size(); ++light)
        {
            if (xpLightType(*light) == 2)
                continue;
            world::LightSource* const source = xpLightSource(*light);
            m_culler->gatherVisibleSphere(&source->getAbsolutePosition().x, xpLightRadius(*light));
            std::vector<TerrainCullerNode*>* const gathered = m_culler->gatheredPatches();
            g_surveyLightPath = 1;                                        // [DEV] terrain.survey
            ++g_surveyLights;
            g_surveyLightNodes += static_cast<unsigned int>(gathered->size());
            addGatheredLight(gathered, source->getAbsolutePosition(),
                             terrainScaledInverseRangeSquared(source->getAttenuationRange2(), rangeScale),
                             source->getColor());
        }
        return;
    }
    static std::vector<world::LightSource*> s_lights;                     // unk_102592A4
    s_lights.clear();
    g_LightManager->getVisibleLightsByMask(s_lights, 2313u, 0u);          // +0x18
    for (std::size_t i = 0; i != s_lights.size(); ++i)
    {
        world::LightSource* const light = s_lights[i];
        m_culler->gatherVisibleSphere(&light->getAbsolutePosition().x, light->getVisualBoundingRadius());
        std::vector<TerrainCullerNode*>* const gathered = m_culler->gatheredPatches();
        g_surveyLightPath = 2;                                            // [DEV] terrain.survey
        ++g_surveyLights;
        g_surveyLightNodes += static_cast<unsigned int>(gathered->size());
        const float scaled = rangeScale * light->getAttenuationRange2();
        addGatheredLight(gathered, light->getAbsolutePosition(), 1.0f / (scaled * scaled),
                         light->getColor());
    }
}


namespace {
const std::string& raTerrainTechniqueName()
{
    static const std::string name("RaTerrain");
    return name;
}

// [RENDER] Technique RaTerrain's constants for one patch, its detail layers 0-5 in layer order (layer 6, under water, is a flag). A layer
// whose texture has no atlas cell is left out, as is the debug-hidden one.
void setRaTerrainPatch(Terrain* t, QuadPatch* patch, const TerrainAtlases& atlases)
{
    float top[6][4], side[6][4], tiling[6][4], weight0[6][4], weight1[6][4], atlas[6][4];
    std::memset(weight0, 0, sizeof(weight0));
    std::memset(weight1, 0, sizeof(weight1));
    int count = 0;
    bool underWater = false;
    const int layers = static_cast<int>(patch->detailLayers.size());
    for (int layer = 0; layer < 7 && layer < layers; ++layer)
    {
        const QuadPatchDetailLayer* const detail = patch->detailLayers[layer];
        if (!detail || layer == t->m_hiddenDetailLayer)
            continue;
        if (layer == 6)
        {
            underWater = true;
            continue;
        }
        const int index = detail->detailIndex;
        if (index < 0 || index / 3 > 1 || index >= static_cast<int>(atlases.top.size())
            || index >= static_cast<int>(t->m_detailTextures.size()))
            continue;
        const TerrainDetailTexture* const record = t->m_detailTextures[index];
        const TerrainAtlasEntry& topEntry = atlases.top[index];
        if (!record || topEntry.atlas < 0)
            continue;
        const bool planar = record->planeMap != 0;
        const TerrainAtlasEntry& sideEntry =
            (planar && atlases.side[index].atlas >= 0) ? atlases.side[index] : topEntry;
        const float topCell[4] = { topEntry.scaleU, topEntry.scaleV, topEntry.offsetU, topEntry.offsetV };
        const float sideCell[4] = { sideEntry.scaleU, sideEntry.scaleV, sideEntry.offsetU, sideEntry.offsetV };
        std::memcpy(top[count], topCell, sizeof(topCell));
        std::memcpy(side[count], sideCell, sizeof(sideCell));
        tiling[count][0] = record->sideTiling.x;
        tiling[count][1] = record->sideTiling.y;
        tiling[count][2] = record->topTiling;
        tiling[count][3] = record->yOffset;
        float* const select = (index / 3 == 0) ? weight0[count] : weight1[count];
        select[0] = detail->selectorX;
        select[1] = detail->selectorY;
        select[2] = detail->selectorZ;
        atlas[count][0] = static_cast<float>(topEntry.atlas);
        atlas[count][1] = static_cast<float>(sideEntry.atlas);
        atlas[count][2] = planar ? 1.0f : 0.0f;
        atlas[count][3] = (dx9rend::rendererHexTiling() && !atlases.grid[index]) ? 1.0f : 0.0f;   // contract 30
        ++count;
    }
    const Terrain::RaTerrainHandles& h = t->m_raTerrain;
    param(h.layerCount)->setInt(count);
    param(h.underWater)->setBool(underWater ? 1 : 0);
    if (count)
    {
        const unsigned int n = static_cast<unsigned int>(count);
        param(h.layerTop)->setValue(top, n);
        param(h.layerSide)->setValue(side, n);
        param(h.layerTiling)->setValue(tiling, n);
        param(h.layerWeight0)->setValue(weight0, n);
        param(h.layerWeight1)->setValue(weight1, n);
        param(h.layerAtlas)->setValue(atlas, n);
    }
    param(h.weight[0])->setValue(patch->detailMap[0], 0);
    param(h.weight[1])->setValue(patch->detailMap[1] ? patch->detailMap[1] : patch->detailMap[0], 0);
}
} // anonymous namespace

// PE 0x100DB680, the +0xBC arm's diffuse passes. [RENDER] Technique RaTerrain, one draw per patch
// over its visible nodes: every detail layer from the atlases, fog once, layer 6 folded in, lit
// from the patch's lightmap and its point lights; it tests EQUAL against the prepass's depth.
// Retail drew per patch one Hi_Terrain pass per detail layer on the near nodes (4, 5, 11, 14) and
// the low-detail pass (3) on the far ones, each reading the accumulation target. Without the
// atlases or the technique terrain draws no colour.
void Terrain::drawFinal()
{
    if (m_flag35B)
    {
        // sub_10074300(6, __FILE__, 2557, "") << the message - dice's debug stream, kept as one line.
        ::renddx9abi::logf("[Terrain.cpp:2557] makeDynamicLightmap is obsolete!");
    }
    if (dx9rend::rendererGetHemiRenderMode() == 4)
        dx9rend::stateManagerClear(1u, 0x808080u, 0.0f, 0u);             // sub_100057E0(1, 8421504, ...)
    const TerrainAtlases* const atlases = terrainAtlases();
    if (!atlases || atlases->detail.empty() || !dx9rend::effectHasTechnique(m_effect, raTerrainTechniqueName()))
        return;

    std::vector<TerrainCullerNode*>* const list = m_culler->visiblePatches();
    TerrainCullerNode** const begin = list->data();
    TerrainCullerNode** const end = begin + list->size();
    if (m_compactVertices)
        dx9rend::vertexBufferBindStream(m_baseGridVertexBuffer, 0);
    dx9rend::vertexDeclarationBind(m_vertexDeclaration);
    const int atlasCount = static_cast<int>(atlases->detail.size());
    for (int i = 0; i < 3; ++i)
        param(m_raTerrain.atlas[i])->setValue(terrainAtlasDetailValue(i < atlasCount ? i : 0), 0);

    dx9rend::effectSetTechniqueByName(m_effect, raTerrainTechniqueName());
    dx9rend::effectBegin(m_effect);
    dx9rend::indexBufferBind(m_lodIndexBuffer);
    dx9rend::effectBeginPass(m_effect, 0u);
    TerrainCullerNode** it = begin;
    while (it != end)
    {
        QuadPatch* const patch = (*it)->patch;
        param(m_hTexLayer[0])->setValue(patch->colormap, 0);
        param(m_hTexLayer[1])->setValue(patch->lightmap, 0);
        param(m_hTexLayer[5])->setValue(patch->lowDetailmap, 0);
        const PatchLights* const lights = findPatchLights(patch);
        const int count = lights ? lights->count : 0;
        param(m_hRaTerrainLightCount)->setInt(count);
        if (count)
        {
            param(m_hRaTerrainLightPos)->setValue(lights->position, static_cast<unsigned int>(count));
            param(m_hRaTerrainLightCol)->setValue(lights->color, static_cast<unsigned int>(count));
        }
        setRaTerrainPatch(this, patch, *atlases);
        patch->drawNodes(it);                                             // advances `it`
    }
    dx9rend::effectEnd(m_effect);
}


// the surrounding ring: Shared_SurroundingTerrain, the full declaration, the corner
// grid for the even slots, the side grid for the odd ones.

void Terrain::drawSurrounding()
{
    dx9rend::effectSetTechniqueByName(m_effect, std::string("Shared_SurroundingTerrain"));
    dx9rend::effectBegin(m_effect);
    dx9rend::vertexDeclarationBind(m_surroundingFullDeclaration);
    dx9rend::vertexBufferBindStream(m_cornerVertexBuffer, 0);
    dx9rend::indexBufferBind(m_cornerIndexBuffer);
    dx9rend::vertexBufferBindStream(m_sideVertexBuffer2, 2);
    for (int s = 0; s < 8; s += 2)
        if (m_surrounding[s])
            m_surrounding[s]->draw(0);
    dx9rend::vertexBufferBindStream(m_sideVertexBuffer, 0);
    dx9rend::indexBufferBind(m_sideIndexBuffer);
    for (int s = 1; s < 8; s += 2)
        if (m_surrounding[s])
            m_surrounding[s]->draw(0);
    dx9rend::effectEnd(m_effect);
}


// (slot +0xD8) - the water: the sea level, then the Ra arm when the distant technique
// exists and its ShaderVersion reads 1, else the grid arm.

void Terrain::drawWater()
{
    ::dice::hfe::world::IHeightmapCluster* const cluster = heightmapClusterByName();
    const float seaLevel = cluster->getSeaWaterLevel();
    if (m_waterDistantTechnique && *m_waterDistantTechnique->shaderVersion() == 1)   // sub_1004AFD0 on +0x718
        drawWaterRa(seaLevel);
    else
        drawWaterGrid(seaLevel);
}

// sub_100DBE30 - which water RaShader a cell draws with: the far one outside the margin around
// the camera, else the near one. [RENDER] Retail drew the near cells with the distant shader too
// unless a directional (sun) shadow frustum touched them: the near shader was the one with the
// shadow map. Water now takes its sun shadow from the cascades (RaSunShadow.fxh), and the sun
// shadow maps are gone, so every near cell gets the near shader.
dx9rend::Ra::RaShader* Terrain::pickWaterTechnique(const float* box, const float* camera)
{
    const float margin = m_waterTechniqueMargin;
    if (box[0] - margin >= camera[0] || box[3] + margin <= camera[0]
        || box[1] - margin >= camera[1] || box[4] + margin <= camera[1]
        || box[2] - margin >= camera[2] || box[5] + margin <= camera[2])
        return m_waterDistantTechnique2;
    return m_waterTechnique;
}

// sub_100DBF50 - the Ra water: the nodes within the far distance of the camera, one patch-sized
// box around each node's patch centre, the technique, the patch's submit.
void Terrain::drawWaterRa(float seaLevel)
{
    const double size = static_cast<double>(m_patchSize);
    const float w = static_cast<float>(size * m_primaryWorldScale.x);
    const float d = static_cast<float>(size * m_primaryWorldScale.z);
    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();        // base +0x50
    float camera[3];
    {
        const float* const m = view->getViewMatrixPtr()->m;
        camera[0] = m[12]; camera[1] = m[13]; camera[2] = m[14];
    }
    view->getFrustumPtr();                                                // answered and dropped, as shipped
    const float* const cameraRow = view->getViewMatrixPtr()->m + 12;
    // [RENDER] With the view-distance cut (render milestone 11 task 5) the sea reaches the horizon:
    // the nodes out to the scenery distance, kept where their water lies in that frustum over
    // terrain that reaches below it, then the sea past the map (drawWaterBeyond). Retail took every
    // node within the view distance, as the frustum ended there.
    const Frustum* const scenery = terrainSceneryFrustum();
    const float radius = scenery ? scenery->getFarDist() : view->getFrustumPtr()->getFarDist();
    m_culler->gatherSphere(cameraRow, radius);
    std::vector<TerrainCullerNode*>* const list = m_culler->gatheredPatches();
    if (scenery)
    {
        const float top = seaLevel + 0.1f, bottom = seaLevel - 0.1f;
        const float cull = seaLevel + m_waterCullHeightOffset;
        list->erase(std::remove_if(list->begin(), list->end(), [&](TerrainCullerNode* node) {
            const TerrainAabb& b = node->bounds;
            const float box[6] = { b.min.x, bottom, b.min.z, b.max.x, top, b.max.z };
            return cull < node->patch->minHeight || !terrainFrustumIntersectsBox(scenery, box);
        }), list->end());
        m_waterObjectRingCursor = 0;
        drawWaterBeyond(seaLevel, scenery);
    }
    TerrainCullerNode** it = list->data();
    if (it == list->data() + list->size())
        return;
    const float hw = w * 0.5f;
    const float hd = d * 0.5f;
    do
    {
        QuadPatch* const patch = (*it)->patch;
        const float cx = patch->center.x;
        const float cz = patch->center.z;
        TerrainAabb box;
        box.min.x = cx - hw; box.min.y = seaLevel - 0.1f; box.min.z = cz - hd;
        box.max.x = hw + cx; box.max.y = seaLevel + 0.1f; box.max.z = hd + cz;
        box.cornerLength = 0.0f; box.radius = 0.0f; box.dirty = 1;
        box.extent.x = 0.0f; box.extent.y = 0.0f; box.extent.z = 0.0f;
        box.centre.x = 0.0f; box.centre.y = 0.0f; box.centre.z = 0.0f;
        raMaterialShaderSlot(*static_cast<RaMaterialParameters*>(m_materialParameters)) =
            pickWaterTechnique(&box.min.x, camera);                       // material +0x14C
        patch->drawWater(it, list, m_materialParameters, m_objectParameters);   // sub_1018D800, advances `it`
    }
    while (it != list->data() + list->size());
}

// [RENDER] The sea past the map (render milestone 11 task 5), in the grid water's cells
// (drawWaterCell) with the surrounding technique. Over each surrounding slot that reaches below the
// sea, one cell; past the ring, rings of eight cells each three times wider than the last, out past
// the scenery distance, each cell drawn when the slot in its direction meets it with sea: the
// median of that slot's outer edge below the sea level. A level without a ring has sea all round
// when its map's lowest point is under the sea level (render milestone 17): the eight cells around
// the map, then the rings past them. With the far ground on (FarGround.h, render milestone 17),
// its heights decide instead, and the surrounding technique lowers the cells by the Earth's
// curvature as the far ground is.
void Terrain::drawWaterBeyond(float seaLevel, const Frustum* frustum)
{
    if (!m_waterSurroundingTechnique)
        return;
    void*& technique = raMaterialShaderSlot(*static_cast<RaMaterialParameters*>(m_materialParameters));   // +0x14C
    const float top = seaLevel + 0.1f, bottom = seaLevel - 0.1f;
    if (farGroundActive())
    {
        // The far ground's heights say where the sea is (render milestone 17 task 3): the eight
        // map-sized cells around the map, then rings of cells three times larger, each drawn
        // where those heights fall under the sea (past their extent, where its edge does).
        const float mapSpan = static_cast<float>(m_patchCount * m_patchSize) * m_primaryWorldScale.x;
        const float reach = frustum->getFarDist() + 1.5f * mapSpan;
        for (float cell = mapSpan; 0.5f * cell < reach; cell *= 3.0f)
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    if (i == 1 && j == 1)
                        continue;
                    const float x0 = (static_cast<float>(i) - 1.5f) * cell, z0 = (static_cast<float>(j) - 1.5f) * cell;
                    const float box[6] = { x0, bottom, z0, x0 + cell, top, z0 + cell };
                    if (!terrainFrustumIntersectsBox(frustum, box) || !farGroundSeaIn(x0, z0, x0 + cell, z0 + cell))
                        continue;
                    technique = m_waterSurroundingTechnique;
                    drawWaterCell(x0, z0, x0 + cell, z0 + cell, 0);
                }
        return;
    }
    float span = 0.0f;
    bool sea[3][3] = {};
    if (!m_surroundingBuilt)
    {
        if (!(m_minHeight < seaLevel))
            return;
        span = static_cast<float>(m_patchCount * m_patchSize) * m_primaryWorldScale.x;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                sea[i][j] = true;
                if (i == 1 && j == 1)
                    continue;
                const float x0 = (static_cast<float>(i) - 1.5f) * span, z0 = (static_cast<float>(j) - 1.5f) * span;
                const float box[6] = { x0, bottom, z0, x0 + span, top, z0 + span };
                if (!terrainFrustumIntersectsBox(frustum, box))
                    continue;
                technique = m_waterSurroundingTechnique;
                drawWaterCell(x0, z0, x0 + span, z0 + span, 0);
            }
    }
    for (int s = 0; s < 8; ++s)
    {
        const SurroundingPatch* const ring = m_surrounding[s];
        if (!ring)
            continue;
        span = static_cast<float>(ring->span);
        const float x0 = static_cast<float>(ring->originX), x1 = static_cast<float>(ring->endX);
        const float z0 = static_cast<float>(ring->originZ), z1 = static_cast<float>(ring->endZ);
        const float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
        sea[cx > 0.0f ? 2 : cx < 0.0f ? 0 : 1][cz > 0.0f ? 2 : cz < 0.0f ? 0 : 1] =
            m_surroundOuterEdge[s] < seaLevel;
        const float box[6] = { x0, bottom, z0, x1, top, z1 };
        if (m_surroundLowest[s] < seaLevel && terrainFrustumIntersectsBox(frustum, box))
        {
            technique = m_waterSurroundingTechnique;
            drawWaterCell(x0, z0, x1, z1, 0);
        }
    }
    if (span <= 0.0f)
        return;
    const float reach = frustum->getFarDist() + 1.5f * span;
    for (float cell = 3.0f * span; 0.5f * cell < reach; cell *= 3.0f)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
            {
                if ((i == 1 && j == 1) || !sea[i][j])
                    continue;
                const float x0 = (static_cast<float>(i) - 1.5f) * cell, z0 = (static_cast<float>(j) - 1.5f) * cell;
                const float box[6] = { x0, bottom, z0, x0 + cell, top, z0 + cell };
                if (!terrainFrustumIntersectsBox(frustum, box))
                    continue;
                technique = m_waterSurroundingTechnique;
                drawWaterCell(x0, z0, x0 + cell, z0 + cell, 0);
            }
    }
}

// sub_100DCA50 - the grid water: every patch-sized cell of the frustum's xz bounds, tested against
// the frustum, its technique picked, its lightmap taken from the patch under it, then one cell
// draw each. The (int) truncations are the shipped FPU control-word conversions.
void Terrain::drawWaterGrid(float seaLevel)
{
    const double size = static_cast<double>(m_patchSize);
    const float w = static_cast<float>(size * m_primaryWorldScale.x);
    const float d = static_cast<float>(size * m_primaryWorldScale.z);
    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();        // base +0x50
    float camera[3];
    {
        const float* const m = view->getViewMatrixPtr()->m;
        camera[0] = m[12]; camera[1] = m[13]; camera[2] = m[14];
    }
    // [RENDER] With the view-distance cut (render milestone 11 task 5) the sea reaches the horizon:
    // the cells of the map in the frustum out to the scenery distance, then the sea past the map
    // (drawWaterBeyond). Retail's cells covered the view frustum's bounds, ending at the view
    // distance in a sawtooth of cells, with the seabed under the haze past it.
    const Frustum* const scenery = terrainSceneryFrustum();
    const Frustum* const frustum = scenery ? scenery : view->getFrustumPtr();
    const float* const fb = reinterpret_cast<const float*>(frustum);      // [72..77]: m_boundingBox min/max
    m_waterObjectRingCursor = 0;
    if (scenery)
        drawWaterBeyond(seaLevel, scenery);
    else if (fb[76] - fb[73] > 100000.0 || fb[77] - fb[74] > 100000.0)
        return;
    const float invW = static_cast<float>(1.0 / w);
    int colStart = static_cast<int>(invW * fb[72]) - 1;
    int colEnd   = static_cast<int>(invW * fb[75]);
    const float invD = static_cast<float>(1.0 / d);
    int rowEnd   = static_cast<int>(invD * fb[77]);
    const double rowF  = fb[74] / d;
    int rowStart = static_cast<int>(rowF) - 1;
    if (scenery)
    {
        // The map's cells only; the frustum's bounds reach tens of kilometres past it.
        const int half = m_patchCount / 2;
        colStart = (std::max)(colStart, -half);
        rowStart = (std::max)(rowStart, -half);
        colEnd = (std::min)(colEnd, m_patchCount - half - 1);
        rowEnd = (std::min)(rowEnd, m_patchCount - half - 1);
    }
    if (rowStart > rowEnd)
        return;
    int rowNext = rowStart + 1;
    for (int row = rowStart; row <= rowEnd; ++row, ++rowNext)
    {
        if (colStart > colEnd)
            continue;
        const float z0 = static_cast<float>(static_cast<double>(row) * d);
        const float z1 = static_cast<float>(static_cast<double>(rowNext) * d);
        const float yMin = static_cast<float>(seaLevel - 0.1);
        const float yMax = static_cast<float>(seaLevel + 0.1);
        int col = colStart;
        int next;
        do
        {
            const float x0 = static_cast<float>(static_cast<double>(col) * w);
            next = col + 1;
            const float x1 = static_cast<float>(static_cast<double>(next) * w);
            TerrainAabb box;
            box.min.x = x0; box.min.y = yMin; box.min.z = z0;
            box.max.x = x1; box.max.y = yMax; box.max.z = z1;
            box.cornerLength = 0.0f; box.radius = 0.0f; box.dirty = 1;
            box.extent.x = 0.0f; box.extent.y = 0.0f; box.extent.z = 0.0f;
            box.centre.x = 0.0f; box.centre.y = 0.0f; box.centre.z = 0.0f;
            box.refresh();                                                // sub_101C5EB0, twice as shipped
            if (box.dirty)
                box.refresh();
            const bool sphereOk = (kWaterCellTests & 1) == 0 || rendmath::frustumInside(*frustum, box.centre, box.radius);
            if (sphereOk && ((kWaterCellTests & 2) == 0 || terrainFrustumIntersectsBox(frustum, &box.min.x)))
            {
                void*& technique =
                    raMaterialShaderSlot(*static_cast<RaMaterialParameters*>(m_materialParameters));   // +0x14C
                technique = pickWaterTechnique(&box.min.x, camera);
                const int c = m_patchCount / 2 + static_cast<int>(invW * x0);
                const int r = m_patchCount / 2 + static_cast<int>(invD * z0);
                void* lightmap;
                if (c < 0 || c >= m_patchCount || r < 0 || r >= m_patchCount)
                {
                    technique = m_waterSurroundingTechnique;
                    lightmap = 0;
                }
                else
                {
                    QuadPatch* const patch = m_patches[c + r * m_patchCount];
                    if (seaLevel + m_waterCullHeightOffset < patch->minHeight)
                    {
                        col = next;
                        continue;
                    }
                    lightmap = patch->lightmap;
                }
                drawWaterCell(x0, z0, x1, z1, lightmap);
            }
            col = next;
        }
        while (next <= colEnd);
    }
}

// sub_100DC400 - one water cell: the 65x65 grid stream object and the 256-entry object-parameter
// ring built on first use, then a render queue entry into RSMTWater with the cell's world matrix,
// the view-projection, the shadow matrices, the lightmap, the water technique and the sky light.
void Terrain::drawWaterCell(float x0, float z0, float x1, float z1, void* lightmap)
{
    namespace Ra = dx9rend::Ra;
    // [CHOSEN, not recovered] Skip excess cells instead of overrunning the ring
    // (issue #33), following marekzajac97/BF2-HUD-fix. Those cells remain undrawn.
    if (m_waterObjectRingCursor >= kWaterObjectRingSize)
        return;
    if (!m_waterGridGeometry)
    {
        // sub_1005F340(140292): a count word, then 256 RaObjectParameters (retail 548 bytes each).
        unsigned char* const ring = static_cast<unsigned char*>(::renddx9abi::engineAlloc(
            static_cast<unsigned int>(kWaterObjectRingHeader + kWaterObjectRingSize * sizeof(Ra::RaObjectParameters))));
        unsigned char* objects = 0;
        if (ring)
        {
            *reinterpret_cast<int*>(ring) = kWaterObjectRingSize;
            objects = ring + kWaterObjectRingHeader;
            for (int i = 0; i < kWaterObjectRingSize; ++i)
                Ra::raObjectParametersConstruct(objects + sizeof(Ra::RaObjectParameters) * i);
        }
        m_waterObjectRing = objects;
        if (!m_waterGridGeometry)
        {
            std::vector<const Ra::RaVertexElementDesc*> elements;
            elements.push_back(&Ra::g_desc_1023EF04);                     // Position
            elements.push_back(&Ra::g_desc_1023ED4C);                     // TBase2D
            elements.push_back(&Ra::g_desc_1023EF2C);                     // TLightMap2D
            void* const storage = ::renddx9abi::engineAlloc(sizeof(Ra::RaStreamDataEditable));   // retail 0x1A4
            Ra::RaStreamDataEditable* const grid = storage
                ? new (storage) Ra::RaStreamDataEditable(4225u, 24576u, 28u, elements, Ra::raRendererDevice())
                : 0;
            m_waterGridGeometryTemp = grid;
            grid->lock();                                                 // vslot +0x24, unguarded as shipped
            // 65 x 65 vertices of (x, 0, z, u, v, u2, v2) over the unit square, in 28-byte records.
            static float vertices[4225 * 7];
            for (int i = 0; i < 4225 * 7; ++i)
                vertices[i] = 0.0f;
            {
                float* v = vertices;
                for (int row = 0; row < 65; ++row)
                {
                    const float zf = static_cast<float>(static_cast<double>(row) * 0.015625);
                    for (int col = 0; col < 65; ++col, v += 7)
                    {
                        const float xf = static_cast<float>(static_cast<double>(col) * 0.015625);
                        v[1] = 0.0f;
                        v[0] = xf;
                        v[2] = zf;
                        v[3] = xf;
                        v[4] = zf;
                        v[5] = xf;
                        v[6] = zf;
                    }
                }
            }
            static unsigned short indices[24576];
            {
                unsigned short* p = indices;
                for (int row = 0; row < 64; ++row)
                {
                    const short base = static_cast<short>(65 * row);
                    for (int col = 0; col < 64; ++col)
                    {
                        *p++ = static_cast<unsigned short>(base + col);
                        *p++ = static_cast<unsigned short>(base + col + 1);
                        *p++ = static_cast<unsigned short>(base + col + 66);
                        *p++ = static_cast<unsigned short>(base + col);
                        *p++ = static_cast<unsigned short>(base + col + 66);
                        *p++ = static_cast<unsigned short>(base + col + 65);
                    }
                }
            }
            m_waterGridGeometryTemp->append(vertices, 28, 4225, indices, 24576);   // vslot +0x2C
            m_waterGridGeometryTemp->unlock();                            // vslot +0x28
            m_waterGridGeometry = m_waterGridGeometryTemp;
            m_waterGridGeometryTemp = 0;
            // The element vector's storage is freed as it leaves scope.
        }
    }

    dx9rend::RenderQueueEntry* const entry = dx9rend::renderEntryPoolAcquire();
    entry->materialParameters = m_materialParameters;
    entry->material = m_waterGridGeometry;
    entry->materialParameters = m_materialParameters;                     // stored twice, as shipped
    Ra::RaObjectParameters& op = *reinterpret_cast<Ra::RaObjectParameters*>(
        m_waterObjectRing + sizeof(Ra::RaObjectParameters) * m_waterObjectRingCursor);
    m_waterObjectRingCursor = m_waterObjectRingCursor + 1;

    ::dice::hfe::world::IHeightmapCluster* const cluster = heightmapClusterByName();
    const float seaLevel = cluster->getSeaWaterLevel();
    {
        dx9rend::IRenderView* const view = dx9rend::rendererGetRenderViewDup();    // base +0x50
        ::memcpy(op.viewProjection.value, view->getViewProjectionMatrixPtr()->m, 64);   // +0x84
    }
    float* const world = op.world.value;
    world[0] = 1.0f; world[1] = 0.0f; world[2] = 0.0f; world[3] = 0.0f;
    world[4] = 0.0f; world[5] = 1.0f; world[6] = 0.0f; world[7] = 0.0f;
    world[10] = 1.0f; world[8] = 0.0f; world[9] = 0.0f; world[11] = 0.0f;
    world[15] = 1.0f; world[12] = 0.0f; world[13] = 0.0f; world[14] = 0.0f;
    world[0]  = x1 - x0;
    world[10] = z1 - z0;
    world[12] = x0;
    world[13] = seaLevel;
    world[14] = z0;
    op.tail220 = 0;
    // [RENDER] Retail copied the first directional shadow record's matrices here (see drawWater).
    entry->objectParameters = &op;
    entry->lighting.lightMapOffset.value[0] = 1.0f;
    entry->lighting.lightMapOffset.value[1] = 1.0f;
    entry->lighting.lightMapOffset.value[2] = 0.0f;
    entry->lighting.lightMapOffset.value[3] = 0.0f;
    {
        IUnknown*& slot = *reinterpret_cast<IUnknown**>(&entry->lighting.lightMap.value);
        if (lightmap)
        {
            IUnknown* const base = static_cast<dx9rend::EngineTexture*>(lightmap)->baseTexture;
            if (slot != base)
            {
                if (slot)
                    slot->Release();
                slot = base;
                if (base)
                    base->AddRef();
            }
        }
        else if (slot)
        {
            slot->Release();
            slot = 0;
        }
    }
    entry->shader = static_cast<Ra::RaShader*>(
        raMaterialShaderSlot(*static_cast<RaMaterialParameters*>(m_materialParameters)));   // +0x14C
    if (world::LightSource* const sky = g_LightManager->getSkyLight())    // +0x28
    {
        float* const record = static_cast<float*>(entry->lightArray.lights.value);
        const float* const dir = terrainSkyLightDirection(sky);           // +0x17C
        record[4] = dir[1];
        record[5] = dir[2];
        record[3] = dir[0];
        const float* const colour = terrainSkyLightColor(sky);            // +0x198
        record[6] = colour[0];
        record[7] = colour[1];
        record[8] = colour[2];
    }
    dx9rend::renderQueueAddToSlot(9u, entry);                             // raShaderServer->m_specialManagers[RSMTWater]->+0x04
}

// [DEV] terrain.survey (render milestone 10, task 1): what the one-pass terrain has to cover on
// the loaded level, one "[terrain.survey]" log line per fact. Draws per pass come from
// BF2_DRAW_STATS and GPU time from the timers' terrain spans; this adds what neither records.
namespace {

std::string surveyFormat(unsigned int format)
{
    if (format > 0xFFu)
    {
        char fourcc[5] = { static_cast<char>(format & 0xFFu), static_cast<char>((format >> 8) & 0xFFu),
                           static_cast<char>((format >> 16) & 0xFFu), static_cast<char>(format >> 24), 0 };
        return fourcc;
    }
    char text[16];
    ::sprintf_s(text, "fmt%u", format);
    return text;
}

std::string surveyTexture(const void* texture)
{
    if (!texture)
        return "-";
    const dx9rend::EngineTexture* const t = static_cast<const dx9rend::EngineTexture*>(texture);
    char text[64];
    ::sprintf_s(text, "%ux%u %s m%u", t->width, t->height, surveyFormat(t->format).c_str(),
                t->mipLevels);
    return text;
}

// Distinct "WxH fmt mips" descriptions with how many textures have each.
void surveyTally(std::vector<std::pair<std::string, int> >& tally, const void* texture)
{
    const std::string key = surveyTexture(texture);
    for (std::size_t i = 0; i != tally.size(); ++i)
        if (tally[i].first == key) { ++tally[i].second; return; }
    tally.push_back(std::make_pair(key, 1));
}

void surveyLogTally(const char* what, const std::vector<std::pair<std::string, int> >& tally)
{
    for (std::size_t i = 0; i != tally.size(); ++i)
        ::renddx9abi::logf("[terrain.survey] %s: %d x %s", what, tally[i].second, tally[i].first.c_str());
}

} // namespace

void Terrain::survey()
{
    const int records = static_cast<int>(m_detailTextures.size());
    ::renddx9abi::logf("[terrain.survey] patchSize %d, patches %dx%d, detail textures %d (file %d), "
                       "low-detail mode %d %s",
                       m_patchSize, m_patchCount, m_patchCount, records, m_detailTextureCount,
                       m_lowDetailTextureMode, surveyTexture(m_lowDetailTexture).c_str());
    for (int i = 0; i != records; ++i)
    {
        TerrainDetailTexture* const r = m_detailTextures[i];
        if (!r)
            continue;
        const std::string name = r->getName();
        ::renddx9abi::logf("[terrain.survey] detail %d '%s': %s normal %s side %s%s sideNormal %s "
                           "planar %d env %d topTiling %g sideTiling %g %g yOffset %g",
                           i, name.c_str(), surveyTexture(r->texture).c_str(),
                           surveyTexture(r->normalTexture).c_str(), surveyTexture(r->sideTexture).c_str(),
                           r->sideTexture == r->texture ? " (same)" : "",
                           surveyTexture(r->sideNormalTexture).c_str(), r->planeMap, r->envMap,
                           r->topTiling, r->sideTiling.x, r->sideTiling.y, r->yOffset);
    }

    // Patches: layer counts, the special layers, and the per-patch textures.
    int byCount[9] = {};
    int withPlanar = 0, withEnv = 0, withLayer6 = 0, patches = 0;
    std::vector<int> detailUse(records > 0 ? records : 0, 0);
    std::vector<std::pair<std::string, int> > detailMaps, colormaps, lightmaps, lowDetailmaps;
    const int total = m_patchCount * m_patchCount;
    for (int p = 0; m_patches && p != total; ++p)
    {
        QuadPatch* const patch = m_patches[p];
        if (!patch)
            continue;
        ++patches;
        int layers = 0;
        bool planar = false, env = false;
        for (std::size_t l = 0; l != patch->detailLayers.size(); ++l)
        {
            QuadPatchDetailLayer* const layer = patch->detailLayers[l];
            if (!layer)
                continue;
            ++layers;
            if (l == 6)
                ++withLayer6;
            const int index = layer->detailIndex;
            if (index >= 0 && index < records)
            {
                ++detailUse[index];
                if (m_detailTextures[index] && m_detailTextures[index]->planeMap)
                    planar = true;
                else if (m_detailTextures[index] && m_detailTextures[index]->envMap)
                    env = true;
            }
        }
        ++byCount[layers < 8 ? layers : 8];
        withPlanar += planar ? 1 : 0;
        withEnv += env ? 1 : 0;
        surveyTally(detailMaps, patch->detailMap[0]);
        if (patch->detailMap[1])
            surveyTally(detailMaps, patch->detailMap[1]);
        surveyTally(colormaps, patch->colormap);
        surveyTally(lightmaps, patch->lightmap);
        surveyTally(lowDetailmaps, patch->lowDetailmap);
    }
    ::renddx9abi::logf("[terrain.survey] patches %d; by layer count 0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d; "
                       "with a planar layer %d, an env-mapped layer %d, layer 6 %d",
                       patches, byCount[0], byCount[1], byCount[2], byCount[3], byCount[4], byCount[5],
                       byCount[6], byCount[7], withPlanar, withEnv, withLayer6);
    for (int i = 0; i != records; ++i)
        ::renddx9abi::logf("[terrain.survey] detail %d used by %d patches", i, detailUse[i]);
    surveyLogTally("detailmap", detailMaps);
    surveyLogTally("colormap", colormaps);
    surveyLogTally("lightmap", lightmaps);
    surveyLogTally("lowDetailmap", lowDetailmaps);
    const Rend::EnvMapManager* const envMaps =
        static_cast<const Rend::EnvMapManager*>(terrainEnvMapManager());
    ::renddx9abi::logf("[terrain.survey] env map %s",
                       surveyTexture(envMaps ? envMaps->m_currentTexture : 0).c_str());

    // The last frame: visible nodes and the point-light gather.
    std::vector<TerrainCullerNode*>* const visible = m_culler->visiblePatches();
    static const char* const kPaths[] = { "none", "static frame lights", "visible mask lights" };
    ::renddx9abi::logf("[terrain.survey] frame: visible nodes %u; "
                       "shader version %d; lights %s, %u lights over %u gathered nodes",
                       static_cast<unsigned int>(visible->size()),
                       m_effect ? *dx9rend::effectShaderVersion(m_effect) : -1,
                       kPaths[g_surveyLightPath], g_surveyLights, g_surveyLightNodes);
}

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

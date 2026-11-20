// Light ownership, active-light culling and shadow-map rendering.
// [RETIRE-WHEN: the XpLightManager GPU resources, listener registration, XpLight coordinator and
// complete 189-slot operational surface are recovered]
// Replace remaining boundaries after their resource lifecycle and draw paths are validated.

#ifdef BF2_IMAGE_RENDDX9
#if !defined(_MSC_VER)
// [CHOSEN, not recovered] Compiler-typed native table entries.
#if defined(__APPLE__) && !DEDICATED
#include "../Platform/macOS/Warmup.h"   // [CHOSEN] +warmupLevel (port addition)
#endif
#include "../../RendDX9Abi/NativeVtable.h"
#include "../RendDX9/RendDX9Base.h"
#endif

#include "../../RendDX9Abi/ThunkAbi.h"
#include "XpLightManager.h"
#include <cstdint>
#include <cstdio>
#include "LightMerger.h"
#include "Terrain.h"
#include "HemiMapManager/HemiMapManager.h"
#include "MeshTemplate.h"                     // the geometry template's isWater (+0x50)
#include "StaticMesh.h"                       // staticMeshRootParent - RendDX9's getRootParent
#include "QuadPatch.h"                        // TerrainCuller - the culler slots' +0x0C query object
#include "BoxCorners.h"                       // the eight-corner object box (0x100CE120)

#include "../../RendDX9Abi/BfMemory.h"
#include "../../RendDX9Abi/Frontier.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../Math/Frustum.h"
#include "../RendDX9/RendDX9Math.h"       // the RendDX9.dll Math copies
#include "../GeomMisc/DebugRenderer.h"
#include "../GeomMisc/Debug.h"
#include "../Platform/X87Compat.h"
#include "../RendMisc/IRenderView.h"
#include "../Game/Main/IVideoOptions.h"
#include "../RendDX9/RendDX9Base.h"
#include "../RendDX9/ShaderParameter.h"
#include "../RendDX9/Texture.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"

// HfeAbi.h supplies the same four-slot IBase contract for the retail VC7.1 host ABI.  Reuse that
// declaration while importing the recovered engine class interfaces below; their virtual method
// order is the contract we need, while STL-bearing arguments are bridged explicitly at the few
// cross-compiler call sites.
#include "../Game/Objects/ObjectLayoutAccess.h"
#include "../Game/Objects/IPlayer.h"
#include "../Scene/Object/ObjectTypes/Game/IPlayerControlObject.h"
#include "../Game/Objects/Visual/LightSource.h"
#include "../Game/Objects/Visual/LightSourceTemplate.h"
#include "../Game/Objects/IObjectTemplate.h"
#include "../Physics/ICollisionMesh.h"
#include "../Physics/HeightmapCluster.h"
#include "../Scene/Object/ObjectManager.h"
#include "../Scene/Object/ObjectPredicator.h"
#include "../Scene/Object/ObjectTypes/Base/ObjectTemplateManager.h"
#include "../Scene/Object/PlayerManager.h"
#include "../Core/SettingsRepostitory.h"

#include <algorithm>
#include <cstddef>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>
#include <new>
#include <list>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <string.h>
#include <vector>

namespace dice {
namespace hfe {

namespace world {
class IObject;
class LightSourceTemplate;
}

namespace dx9rend {
IRenderView* rendererGetRenderView();
bool rendererIsExpansionPack();
IVideoOptions* rendererGetVideoOptions();
extern void* g_rendererBase;
extern void* g_shaderManager;
}

namespace world {
// RendDX9 is a separate shipped image from BF2.exe. Both ABI builds now construct typed
// BundledMesh collision filters and need this non-pure base slot. PE1012C350 / ELF8163FC0
// are unconditional true; emit the recovered body locally as the original DLL did.
#if !(defined(__APPLE__) && defined(BF2_STATIC_PLUGINS))
bool ObjectPredicator::predicateSurface(int)
{
    return true;
}
#else
// [CHOSEN, not recovered] The static image shares this class's host key function/RTTI.
#endif
}

extern IBase* objectManager;
extern IBase* playerManager;

namespace geom {

extern LightManager* g_LightManager;
extern IBase* g_DebugRenderer;
// dword_1024B930, ModuleGeom.cpp's Terrain singleton.
extern IBase* g_Terrain;

extern const unsigned int CID_LightManager  = 852417u;
extern const unsigned int IID_ILightManager = 0xB801DCF9u;
namespace {

extern const void* const kPrimaryVtable[189];
extern const void* const kRenderPathVtable[6];

// The PE's 0x18-byte-node trees (allocator 0x100F5CF0): an unsigned key and a pointer value.
typedef std::map<unsigned int, void*> ShadowUnresolvedMap;   // [UNRESOLVED] value types

// The shader-parameter handles of "Shaders/LightManager_r3x0", +0x848..+0x8B4 in the PE.
enum LightShaderParameter
{
    kParamStencilFunc,              // +0x848  STENCILFUNC
    kParamStencilRef,               // +0x84C  STENCILREF
    kParamStencilPass,              // +0x850  STENCILPASS
    kParamParaboloidZValues,        // +0x854  PARABOLOIDZVALUES
    kParamLightWorldPos,            // +0x858  LIGHTWORLDPOS
    kParamLightDir,                 // +0x85C  LIGHTDIR
    kParamLightPos,                 // +0x860  LIGHTPOS
    kParamLightCol,                 // +0x864  LIGHTCOL
    kParamLightAttenuationRange,    // +0x868  LIGHTATTENUATIONRANGE
    kParamLightAttenuationRangeInv, // +0x86C  LIGHTATTENUATIONRANGEINV
    kParamProjectorMask,            // +0x870  PROJECTORMASK
    kParamEyePos,                   // +0x874  EYEPOS
    kParamEyeDof,                   // +0x878  EYEDOF
    kParamViewProj,                 // +0x87C  VIEWPROJ
    kParamWorldViewProj,            // +0x880  WORLDVIEWPROJ
    kParamLightViewProj,            // +0x884  LIGHTVIEWPROJ
    kParamLightOccluderViewProj,    // +0x888  LIGHTOCCLUDERVIEWPROJ
    kParamViewportMap,              // +0x88C  VIEWPORTMAP
    kParamViewportMap2,             // +0x890  VIEWPORTMAP2
    kParamShadowIntensityBias,      // +0x894  SHADOWINTENSITYBIAS
    kParamLightMapIntensityBias,    // +0x898  LIGHTMAPINTENSITYBIAS
    kParamTexLayer0,                // +0x89C  TEXLAYER0 .. +0x8B4 TEXLAYER6
    kParamTexLayer1,
    kParamTexLayer2,
    kParamTexLayer3,
    kParamTexLayer4,
    kParamTexLayer5,
    kParamTexLayer6,
    kLightShaderParameterCount
};

// The LightManager object (0x980 bytes in the PE, RefFactory_geom_LightManager; the `// +0xNN`
// comments keep the retail offsets). Hand-built tables sit at its start: the primary interface and
// the RenderPathListener subobject ([RENDER] retail's DeviceLostResetListener subobject at +0x004
// only rebuilt the point/spot shadow maps and is deleted with them). The
// default constructor builds the containers only; constructLightManager writes every field the
// PE constructor writes, in its order, and leaves the rest as allocated. Names are [INFERRED]
// except the console properties'.
struct LightManagerData
{
    LightManagerData() {}

    const void* const*         primaryVtable;                  // +0x000 kPrimaryVtable
    const void* const*         renderPathVtable;               // +0x008 the listener subobject
    void*                      renderPathValue;                // +0x00C the render-path listener's
                                                               //        getRenderPath; NOT constructed
    int                        refCount;                       // +0x010
    world::LightSource*        skyLight;                       // +0x014
    Vec3                       skyColor;                       // +0x018
    Vec3                       treeSunColor;                   // +0x024
    Vec3                       treeSkyColor;                   // +0x030
    Vec3                       ambientColor;                   // +0x03C
    Vec3                       treeAmbientColor;               // +0x048
    Vec3                       staticSunColor;                 // +0x054
    Vec3                       staticSkyColor;                 // +0x060
    Vec3                       staticSpecularColor;            // +0x06C
    Vec3                       singlePointColor;               // +0x078
    Vec3                       dynamicPointColor;              // +0x084
    Vec3                       blendedColors[12];              // +0x090 high/low colour pairs
    Vec3                       effectSunColor;                 // +0x120
    Vec3                       effectShadowColor;              // +0x12C
    float                      defaultEffectLightAffectionFactor; // +0x138
    float                      lightCullDistance;              // +0x13C
    int                        maxLightsPerBundledMesh;        // +0x140
    int                        maxLightsPerStaticMesh;         // +0x144
    int                        maxLightsPerSkinnedMesh;        // +0x148
    int                        maxLightsPerTerrainPatch;       // +0x14C
    unsigned int               terrainShadowLod;                  // +0x150 1
    unsigned int               staticMeshShadowLod;            // +0x154
    int                        bundledMeshShadowLod;           // +0x158
    int                        skinnedMeshShadowLod;           // +0x15C
    int                        shadowOverrideQuality;                  // +0x160 -1
    float                      shadowRoofRayLength;            // +0x164
    float                      shadowSunScalePower;            // +0x168
    float                      shadowPullbackDistance;         // +0x16C the pull toward the light
    unsigned char              shadowUseSweepTest;             // +0x170
    Vec2                       shadowSunHeightOverFade;        // +0x174
    // +0x17C / +0x180 - SimpleShadowmapIO.raw as a vector<bool>: the bit count, then the words.
    unsigned int               simpleShadowIOBitCount;         // +0x17C
    std::vector<unsigned int>  simpleShadowIOWords;            // +0x180
    // +0x190..+0x1B0 - SimpleShadowmap.raw packed two bits per texel, and both maps' geometry.
    unsigned char*             simpleShadowMap;                // +0x190
    float                      simpleShadowLimit;              // +0x194 half the map, the clamp
    int                        simpleShadowIOStride;           // +0x198
    int                        simpleShadowIOColumnOrigin;     // +0x19C
    int                        simpleShadowRowCount;           // +0x1A0
    int                        simpleShadowIORowOrigin;        // +0x1A4
    int                        simpleShadowStride;             // +0x1A8 bytes per packed row
    int                        simpleShadowColumnOrigin;       // +0x1AC
    int                        simpleShadowRowOrigin;          // +0x1B0
    float                      skinnedMeshShaderLodDistance;   // +0x1B4
    std::set<world::LightSource*> ownedLights;                 // +0x1B8 addLight's, owning
    std::set<world::LightSource*> activeLights;                // +0x1C4 enableLight's, non-owning
    std::vector<world::LightSource*> visibleLights;            // +0x1D0 update's frame list
    std::vector<world::LightSource*> distanceLights;           // +0x1E0
    LightDistanceMap          lightsByDistance;               // +0x1F0
    Vec2                       lightMapAmbientAlphaCutoffRange; // +0x1FC
    int                        shadowMapSize;                  // +0x204
    int                        shadowTileSize;                 // +0x208
    unsigned int               shadowMapFormat;                // +0x20C
    unsigned int               shadowMapUsage;                 // +0x210
    float                      shadowDistance;                 // +0x214 the camera frustum's far
    float                      unresolved218;                  // +0x218 40; the +0xBC/+0xC0 pair's field
    dx9rend::EngineTexture*    whiteTexture;                   // +0x23C
    dx9rend::EngineTexture*    blackTexture;                   // +0x240
    dx9rend::EngineTexture*    framedTexture;                  // +0x244
    dx9rend::EngineTexture*    blackCubeTexture;               // +0x248
    dx9rend::EngineTexture*    normalCubeTexture;              // +0x24C
    dx9rend::EngineTexture*    currentSpecularLut;             // +0x250
    dx9rend::EngineTexture*    specularLuts[4];                // +0x254 the four-LUT ring
    unsigned int               specularLutIndex;               // +0x264
    unsigned char              shadowStaticFeature;            // +0x268
    float                      shadowIntensityBias;            // +0x26C
    float                      lightMapIntensityBias;          // +0x270
    float                      dropShadowClipHeightOffset;     // +0x274
    Frustum                    shadowFrustum;                  // +0x278 the light-space frustum
    Frustum                    cameraFrustum;                  // +0x460
    Mat4                       unresolvedMatrices648[5];       // +0x648 identity [UNRESOLVED]
    unsigned char              showFrustums;                   // +0x788 the debug draw byte
    unsigned char              dynamicShadowsEnabled;          // +0x7EC
    unsigned char              firstPersonShadows;             // +0x7ED
    unsigned char              dynamicStaticMeshShadowsEnabled;                  // +0x7EE 1
    unsigned char              allowAlphaShadows;                  // +0x7EF 0
    float                      alphaShadowThreshold;           // +0x7F0 0.2
    float                      shadowUpdateFactor;                  // +0x7F4 1
    unsigned char              stencilCullerResetEnabled;      // +0x7F8 1
    unsigned char              shadowUpdatesEnabled;           // +0x7F9
    float                      shadowOccluderOversizeFactor;                  // +0x7FC 1
    unsigned char              shadowProjectionFlags[4];               // +0x800 0, 0, 1, 1
    float                      shadowPSMZNearOffset;           // +0x804
    float                      shadowPSMZFarOffset;            // +0x808
    float                      shadowPSMZMinInfinityZ;                  // +0x80C 1.5
    float                      shadowPSMFovY;                  // +0x810 60
    float                      shadowTSMFocus;                 // +0x814
    float                      shadow80Line;                  // +0x818 -0.6
    unsigned char              shadowMergeDynamicShadowFrustums;                  // +0x81C 1
    unsigned int               shadowMergeLodLimit;            // +0x820
    float                      dynamicLightRangeScale;         // +0x824
    float                      occludedSkyNightIntensity;      // +0x828
    // +0x82C / +0x830 - D3DRS_DEPTHBIAS and D3DRS_SLOPESCALEDEPTHBIAS, handed to the state manager
    // as their float bit patterns.
    float                      shadowDepthBias;                // +0x82C
    float                      shadowSlopeScaleDepthBias;      // +0x830
    double                     shadowFadeDuration;             // +0x838
    world::LightSource*        sunLight;                       // +0x840 the "sun" initialize creates
    dx9rend::Effect*           shadowEffect;                   // +0x844 Shaders/LightManager_r3x0
    dx9rend::ShaderParameter*  shaderParameters[kLightShaderParameterCount]; // +0x848
    dx9rend::EngineTexture*    radialAttenuation;              // +0x8B8 RadialAtt2
    dx9rend::EngineTexture*    conicalAttenuation;             // +0x8BC ConicalAtt
    ShadowUnresolvedMap        unresolvedTree8C0;              // +0x8C0 [UNRESOLVED]
    IDirect3DVertexBuffer9*    sphereVertices;                 // +0x8D8 the debug meshes
    IDirect3DIndexBuffer9*     sphereIndices;                  // +0x8DC
    unsigned int               sphereVertexCount;              // +0x8E0
    unsigned int               sphereFaceCount;                // +0x8E4
    IDirect3DVertexBuffer9*    cylinderVertices;               // +0x8E8
    IDirect3DIndexBuffer9*     cylinderIndices;                // +0x8EC
    unsigned int               cylinderVertexCount;            // +0x8F0
    unsigned int               cylinderFaceCount;              // +0x8F4
    IDirect3DVertexBuffer9*    boxVertices;                    // +0x8F8
    IDirect3DIndexBuffer9*     boxIndices;                     // +0x8FC
    unsigned int               boxVertexCount;                 // +0x900
    unsigned int               boxFaceCount;                   // +0x904
    IDirect3DDevice9*          device;                         // +0x908
    unsigned int               shadowMapMode;                  // +0x910 0 nvidia depth, 1 colour, 2 none
    std::vector<world::IObject*> shadowCandidates;             // +0x914
    std::vector<void*>         directionalGeometry;            // +0x924
    std::vector<world::IObject*> staticShadowCasters;          // +0x934
    std::vector<void*>         unresolvedGeometry944;          // +0x944 cleared only [UNRESOLVED]
    std::vector<world::IObject*> directionalChildren;          // +0x954
    std::vector<void*>         overGrowthGeometry;             // +0x970

private:
    LightManagerData(const LightManagerData&);
    LightManagerData& operator=(const LightManagerData&);
};

inline LightManagerData& lm(void* self)
{
    return *static_cast<LightManagerData*>(self);
}

inline const LightManagerData& lm(const void* self)
{
    return *static_cast<const LightManagerData*>(self);
}

// The listener subobject hands its own address to the callbacks; the owner is that address less
// the subobject's position.
inline void* lmFromRenderPathListener(void* listener)
{
    return static_cast<unsigned char*>(listener) - offsetof(LightManagerData, renderPathVtable);
}

// The bit pattern of a float, as the depth-bias render states receive it.
inline unsigned int floatBits(float value)
{
    unsigned int bits;
    ::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

// PE 0x101C3F70 (ecx = texture, edx = count; exponent, &rgb, save on the stack). One A8R8G8B8 row:
// texel i = 255 * pow(i / (count - 1), exponent) per channel, truncated (fistp under a chop control
// word), alpha 0xFF. The power is the CRT's double _CIpow of the exact i * scale product; red keeps
// value * rgb.x on the x87 stack, green and blue store value * rgb as floats first. The count is a
// signed int. With `save` the row is written to "c:\lut_<exponent>_<texture>.dds" - the second %i
// receives the texture pointer, a shipped quirk; no 1.50 caller sets it.
void fillPowerLut(dx9rend::EngineTexture* texture, int count,
                  const Vec3& rgb, float exponent, bool save)
{
    dx9rend::TextureLockRecord* const lock =
        dx9rend::textureLock(texture, 0, 0x7FFFFFFFu);
    unsigned int* pixel = static_cast<unsigned int*>(lock->pBits);
    const float scale = 1.0f / static_cast<float>(count - 1);
    for (int i = 0; i < count; ++i)
    {
        const double value = std::pow(static_cast<double>(i) * scale, static_cast<double>(exponent));
        const float green = static_cast<float>(value * rgb.y);
        const float blue = static_cast<float>(value * rgb.z);
#if defined(__APPLE__)
        // [CHOSEN, not recovered] the x87 red product and the fistp conversions.
        using namespace bf2x87;
        const int red = toIntTruncate(mul(wide(255.0f), mul(wide(value), wide(rgb.x))));
        const int greenByte = toIntTruncate(mul(wide(green), wide(255.0f)));
        const int blueByte = toIntTruncate(mul(wide(blue), wide(255.0f)));
#else
        const int red = static_cast<int>(255.0 * (value * rgb.x));
        const int greenByte = static_cast<int>(static_cast<double>(green) * 255.0);
        const int blueByte = static_cast<int>(static_cast<double>(blue) * 255.0);
#endif
        *pixel++ = (((((static_cast<unsigned int>(red) | 0xFFFFFF00u) << 8)
                    | (static_cast<unsigned int>(greenByte) & 0xFFu)) << 8)
                    | (static_cast<unsigned int>(blueByte) & 0xFFu));
    }
    dx9rend::textureUnlock(texture, lock);
    if (save)
    {
        char buffer[0x1000];
        std::snprintf(buffer, 0xFFF, "c:\\lut_%i_%i.dds", static_cast<int>(exponent),
                      static_cast<int>(reinterpret_cast<std::intptr_t>(texture)));
        dx9rend::textureManagerSaveTexture(texture, std::string(buffer));
    }
}

// PE 0x101C3DF0 (ecx = count, edx = &rgb; exponent, save on the stack): a count x 1 A8R8G8B8 texture
// (one level, default pool), then the fillPowerLut body inline.
dx9rend::EngineTexture* createPowerLut(int count, const Vec3& rgb,
                                      float exponent, bool save)
{
    dx9rend::EngineTexture* const texture =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateEmpty(
            count, 1, D3DFMT_A8R8G8B8, 1, 0, 0));
    fillPowerLut(texture, count, rgb, exponent, save);
    return texture;
}

int shadowNormalizeInPlace(Vec3& value);

// PE 0x101C3A30.
inline unsigned int packNormalDirection(const Vec3& value)
{
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Each floor argument is stored as binary64 by the x87 caller.
    const int red = bf2x87::toIntTruncate(bf2x87::wide(std::floor(
        bf2x87::storeDouble(bf2x87::wide(value.x) * 127.0f + 0.5f))) + 128.0f);
    const int green = bf2x87::toIntTruncate(bf2x87::wide(std::floor(
        bf2x87::storeDouble(bf2x87::wide(value.y) * 127.0f + 0.5f))) + 128.0f);
    const int blue = bf2x87::toIntTruncate(bf2x87::wide(std::floor(
        bf2x87::storeDouble(bf2x87::wide(value.z) * 127.0f + 0.5f))) + 128.0f);
#else
    const int red = static_cast<int>(std::floor(value.x * 127.0f + 0.5f) + 128.0f);
    const int green = static_cast<int>(std::floor(value.y * 127.0f + 0.5f) + 128.0f);
    const int blue = static_cast<int>(std::floor(value.z * 127.0f + 0.5f) + 128.0f);
#endif
    return 0xFF000000u | (static_cast<unsigned int>(red) << 16)
         | (static_cast<unsigned int>(green) << 8) | static_cast<unsigned int>(blue);
}

// PE 0x101C38E0.
void normalCubeDirection(Vec3& direction, unsigned int face, unsigned int side,
                         unsigned int x, unsigned int y)
{
    const float denominator = static_cast<float>(side - 1);
#if defined(__APPLE__)
    // [CHOSEN, not recovered] The integer loads and quotient stay extended until each coordinate store.
    const float u = bf2x87::store(bf2x87::wide(static_cast<int>(x)) / denominator * 2.0f - 1.0f);
    const float v = bf2x87::store(bf2x87::wide(static_cast<int>(y)) / denominator * 2.0f - 1.0f);
#else
    const float u = static_cast<float>(x) / denominator * 2.0f - 1.0f;
    const float v = static_cast<float>(y) / denominator * 2.0f - 1.0f;
#endif
    switch (face)
    {
    case 0: direction.x =  1.0f; direction.y = -v;   direction.z = -u;   break;
    case 1: direction.x = -1.0f; direction.y = -v;   direction.z =  u;   break;
    case 2: direction.x =  u;    direction.y =  1.0f; direction.z =  v;   break;
    case 3: direction.x =  u;    direction.y = -1.0f; direction.z = -v;   break;
    case 4: direction.x =  u;    direction.y = -v;   direction.z =  1.0f; break;
    case 5: direction.x = -u;    direction.y = -v;   direction.z = -1.0f; break;
    }
    shadowNormalizeInPlace(direction);
}

// PE 0x101C36E0: cube lookup type 1 (ecx = side, eax = face, esi = out, edi = &offset; x, y on the
// stack). The face direction of normalCubeDirection (side, x and y are signed here: fild), then
// normalize(direction + offset), written back in place. Unreachable in 1.50 as a direct call: only
// fillNormalCubeMip's type-1 arm reaches it, and the sole retail caller asks for type 3.
// [CHOSEN, not recovered] plain float arithmetic: the retail x87 keeps u in extended precision,
// but no 1.50 path executes the body.
void normalCubeOffsetDirection(Vec3& direction, const Vec3& offset, unsigned int face,
                               int side, int x, int y)
{
    const float denominator = static_cast<float>(side - 1);
    const float u = static_cast<float>(x) / denominator * 2.0f - 1.0f;
    const float v = static_cast<float>(y) / denominator * 2.0f - 1.0f;
    switch (face)
    {
    case 0: direction.x =  1.0f; direction.y = -v;   direction.z = -u;   break;
    case 1: direction.x = -1.0f; direction.y = -v;   direction.z =  u;   break;
    case 2: direction.x =  u;    direction.y =  1.0f; direction.z =  v;   break;
    case 3: direction.x =  u;    direction.y = -1.0f; direction.z = -v;   break;
    case 4: direction.x =  u;    direction.y = -v;   direction.z =  1.0f; break;
    case 5: direction.x = -u;    direction.y = -v;   direction.z = -1.0f; break;
    }
    shadowNormalizeInPlace(direction);
    direction.x = offset.x + direction.x;
    direction.y = offset.y + direction.y;
    direction.z = offset.z + direction.z;
    shadowNormalizeInPlace(direction);
}

// PE 0x101C37E0: cube lookup type 2, same register contract as 0x101C36E0. The face direction of
// normalCubeDirection, normalized, then replaced by the scalar (offset.y * d.y + offset.z * d.z) +
// offset.x * d.x stored in all three components. Unreachable in 1.50 as a direct call: only
// fillNormalCubeMip's type-2 arm reaches it.
void normalCubeDotDirection(Vec3& direction, const Vec3& offset, unsigned int face,
                            int side, int x, int y)
{
    const float denominator = static_cast<float>(side - 1);
    const float u = static_cast<float>(x) / denominator * 2.0f - 1.0f;
    const float v = static_cast<float>(y) / denominator * 2.0f - 1.0f;
    switch (face)
    {
    case 0: direction.x =  1.0f; direction.y = -v;   direction.z = -u;   break;
    case 1: direction.x = -1.0f; direction.y = -v;   direction.z =  u;   break;
    case 2: direction.x =  u;    direction.y =  1.0f; direction.z =  v;   break;
    case 3: direction.x =  u;    direction.y = -1.0f; direction.z = -v;   break;
    case 4: direction.x =  u;    direction.y = -v;   direction.z =  1.0f; break;
    case 5: direction.x = -u;    direction.y = -v;   direction.z = -1.0f; break;
    }
    shadowNormalizeInPlace(direction);
    const float dot = (offset.y * direction.y + offset.z * direction.z) + offset.x * direction.x;
    direction.z = dot;
    direction.y = dot;
    direction.x = dot;
}

// PE 0x101C39F0 (ecx = texture; also inlined at the tail of fillNormalCubeMip): the debug save of a
// cube texture to "c:\\cubedump_<texture>.dds", the %i receiving the texture pointer as in
// fillPowerLut. Always answers true. Unreachable in 1.50: no code or data references to the
// standalone copy.
bool saveCubeDump(dx9rend::EngineTexture* texture)
{
    char buffer[0x1000];
    std::snprintf(buffer, 0xFFF, "c:\\cubedump_%i.dds",
                  static_cast<int>(reinterpret_cast<std::intptr_t>(texture)));
    dx9rend::textureManagerSaveTexture(texture, std::string(buffer));
    return true;
}

// PE 0x101C3B00 (ecx = texture, edx = mip; side, &light, save and lookup type on the stack, retn
// 0x10). The light direction is normalized in place first. Type 1 formats each texel through
// 0x101C36E0, type 2 through 0x101C37E0, type 3 through normalCubeDirection (the only type a
// 1.50 caller passes); any other type reports the BumpmapUtil.cpp error and packs the previous
// direction. It locks each cube surface directly and deliberately advances one dword per texel
// without consulting Pitch.
bool fillNormalCubeMip(dx9rend::EngineTexture* texture, unsigned int mip, int side,
                       Vec3& light, bool save, int lookupType)
{
    Vec3 direction = { 0.0f, 0.0f, 0.0f };
    shadowNormalizeInPlace(light);
    for (unsigned int face = 0; face != 6; ++face)
    {
        IDirect3DSurface9* const surface =
            dx9rend::textureGetLevelSurface(texture, mip, face);
        D3DLOCKED_RECT locked;
        surface->LockRect(&locked, 0, 0);
        unsigned int* output = static_cast<unsigned int*>(locked.pBits);
        for (int y = 0; y < side; ++y)
        {
            for (int x = 0; x < side; ++x)
            {
                switch (lookupType)
                {
                case 1:
                    normalCubeOffsetDirection(direction, light, face, side, x, y);   // PE 0x101C36E0
                    break;
                case 2:
                    normalCubeDotDirection(direction, light, face, side, x, y);      // PE 0x101C37E0
                    break;
                case 3:
                    normalCubeDirection(direction, face, static_cast<unsigned int>(side),
                                        static_cast<unsigned int>(x),
                                        static_cast<unsigned int>(y));
                    break;
                default:
                    Debug(DebugTypeError,
                          "C:\\dice\\Projects\\BF2Branches\\Patch_1_50\\Code\\BF2\\RendMisc\\BumpmapUtil.cpp",
                          0x1AC, "") << "unknown cubemap lookup type specified!!!";
                    break;
                }
                *output++ = packNormalDirection(direction);
            }
        }
        surface->UnlockRect();
        surface->Release();
    }
    if (save)
        saveCubeDump(texture);                       // inlined copy of PE 0x101C39F0
    return true;
}

// PE 0x101C3CD0 (ecx = side, edx = &light; save on the stack): a one-level A8R8G8B8 cube texture
// filled with lookup type 1. Unreachable in 1.50: no code or data references.
dx9rend::EngineTexture* createNormalCubeTextureType1(int side, Vec3& light, bool save)
{
    dx9rend::EngineTexture* const texture =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateCube(
            side, D3DFMT_A8R8G8B8, 1, 0, 0));
    fillNormalCubeMip(texture, 0, side, light, save, 1);
    return texture;
}

// PE 0x101C3D10: as 0x101C3CD0 with lookup type 2. Unreachable in 1.50: no code or data references.
dx9rend::EngineTexture* createNormalCubeTextureType2(int side, Vec3& light, bool save)
{
    dx9rend::EngineTexture* const texture =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateCube(
            side, D3DFMT_A8R8G8B8, 1, 0, 0));
    fillNormalCubeMip(texture, 0, side, light, save, 2);
    return texture;
}

// The ATI/PS1 combination receives one mip; every other device receives three.
// PE 0x101C3D50 (ecx = side, edx = &light, save on the stack); the sole caller
// (lm_setSunDirection) supplies side 256, a zero light vector, save false and, internally, lookup
// type 3.
dx9rend::EngineTexture* createNormalCubeTexture(int side, Vec3& light, bool save)
{
    const unsigned int mipLevels =
        dx9rend::deviceCapsIsAti() && dx9rend::deviceCapsPixelShaderMajor() == 1 ? 1u : 3u;
    dx9rend::EngineTexture* const texture =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateCube(
            side, D3DFMT_A8R8G8B8, mipLevels, 0, 0));
    fillNormalCubeMip(texture, 0, side, light, save, 3);
    if (mipLevels > 1)
    {
        fillNormalCubeMip(texture, 1, side >> 1, light, save, 3);
        fillNormalCubeMip(texture, 2, side >> 2, light, save, 3);
    }
    return texture;
}

void createSmallLightTextures(void* self)
{
    dx9rend::EngineTexture* const white =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateEmpty(
            8, 8, D3DFMT_A8R8G8B8, 1, 0, 0));
    lm(self).whiteTexture = white;
    dx9rend::TextureLockRecord* lock = dx9rend::textureLock(white, 0, 0x7FFFFFFFu);
    ::memset(lock->pBits, 0xFF, (lock->bpp / 8) << 6);
    dx9rend::textureUnlock(white, lock);

    dx9rend::EngineTexture* const black =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateEmpty(
            8, 8, D3DFMT_A8R8G8B8, 1, 0, 0));
    lm(self).blackTexture = black;
    lock = dx9rend::textureLock(black, 0, 0x7FFFFFFFu);
    ::memset(lock->pBits, 0, (lock->bpp / 8) << 6);
    dx9rend::textureUnlock(black, lock);

    dx9rend::EngineTexture* const framed =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateEmpty(
            8, 8, D3DFMT_A8R8G8B8, 1, 0, 0));
    lm(self).framedTexture = framed;
    lock = dx9rend::textureLock(framed, 0, 0x7FFFFFFFu);
    ::memset(lock->pBits, 0xFF, (lock->bpp / 8) << 6);
    const unsigned int bytesPerPixel = static_cast<unsigned int>(lock->bpp / 8);
    unsigned char* const bits = static_cast<unsigned char*>(lock->pBits);
    for (unsigned int y = 2; y != 6; ++y)
    {
        for (unsigned int x = 2; x != 6; ++x)
            *reinterpret_cast<unsigned int*>(bits + (y * 8 + x) * bytesPerPixel) = 0xFF000000u;
    }
    dx9rend::textureUnlock(framed, lock);

    dx9rend::EngineTexture* const cube =
        static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerCreateCube(
            8, D3DFMT_X8R8G8B8, 1, 0, 0));
    lm(self).blackCubeTexture = cube;
    IDirect3DSurface9* const source =
        dx9rend::textureGetLevelSurface(black, 0, 0x7FFFFFFFu);
    for (unsigned int face = 0; face < 6; ++face)
    {
        IDirect3DSurface9* const destination =
            dx9rend::textureGetLevelSurface(cube, 0, face);
        D3DXLoadSurfaceFromSurface(destination, 0, 0, source, 0, 0, 0xFFFFFFFFu, 0);
        destination->Release();
    }
    source->Release();
}


// PE 0x100057A0: substitutes Renderer +0x770 when passed null (this is not equivalent to handing a
// literal null to StateManager +0x64), then calls StateManager +0x64 and returns true (al = 1);
// every caller discards the result.
bool setLightManagerDepthStencil(IDirect3DSurface9* surface)
{
    const bool shadowDepth = surface != 0;
    if (!surface)
        surface = dx9rend::rendererDepthStencilSurface();                  // +0x770
    dx9rend::stateManagerSetDepthStencilSurface(surface);
    // [RENDER] reversed-Z: shadow maps keep forward depth (their own light matrices)
    dx9rend::stateManagerMarkStandardDepth(shadowDepth);
    return true;
}

void restoreRendererTarget0()
{
    // PE 0x10028070 first restores Renderer +0x7D4's EngineTexture. Only a null texture takes
    // the generic 0x10005920 back-buffer fallback of lightManagerSelectRenderTarget.
    dx9rend::EngineTexture* const texture = dx9rend::rendererPathTarget(0x7D4);
    lightManagerSetRenderTargetTexture(0, texture);
}

// Calls into BF2.exe use the recovered source interfaces themselves.  HostString is VC7.1's
// std::string layout in retail mode, but these callees receive it by reference, so the bridge only
// changes the static spelling of that reference; no current-CRT string operation touches it.
inline const std::string& hostStdString(const renddx9abi::HostString& value)
{
    return value;
}

inline const world::LightSourceTemplate* lightSourceTemplatePointer(
    const world::LightSource* light)
{
    return static_cast<const world::LightSourceTemplate*>(light->getTemplate());
}

inline unsigned int lightSourceFlags(const world::LightSource* light)
{
    return light->getFlags();
}

inline const world::IObjectTemplate* objectTemplatePointer(const world::IObject* object)
{
    return object->getTemplate();
}

void* getEffectParameter(void* effect, const char* semantic)
{
    std::string name(semantic);
    return dx9rend::effectGetShaderParameter(
        static_cast<dx9rend::Effect*>(effect), name);
}

dx9rend::EngineTexture* loadManagerTexture(const char* path)
{
    std::string name(path);
    return static_cast<dx9rend::EngineTexture*>(dx9rend::textureManagerGetTexture(name));
}


// Normalizes in place unless already within FLT_EPSILON of unit length.
// Degenerate vectors become zero.
// PE 0x101682C0, identical to the shared PE 0x100777A0 instantiation.
int shadowNormalizeInPlace(Vec3& v)
{
#if defined(__APPLE__)
    // [CHOSEN, not recovered] PE 0x100777A0 stores the sum, retaining the reciprocal for all stores.
    const float lengthSquared = bf2x87::store(bf2x87::wide(v.x) * v.x
        + bf2x87::wide(v.y) * v.y + bf2x87::wide(v.z) * v.z);
#else
    const float lengthSquared = v.x * v.x + v.y * v.y + v.z * v.z;
#endif
    if (std::fabs(lengthSquared - 1.0f) < FLT_EPSILON)
        return 0;
    if (std::fabs(lengthSquared) < FLT_EPSILON)
    {
        v.x = 0.0f;
        v.y = 0.0f;
        v.z = 0.0f;
        return -20001;
    }
#if defined(__APPLE__)
    const bf2x87::Real inverse = bf2x87::wide(1.0f) / bf2x87::squareRoot(bf2x87::wide(lengthSquared));
    v.x = bf2x87::store(inverse * v.x);
    v.y = bf2x87::store(inverse * v.y);
    v.z = bf2x87::store(inverse * v.z);
#else
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    v.x *= inverse;
    v.y *= inverse;
    v.z *= inverse;
#endif
    return 0;
}


struct XpCuller
{
    const void** vtable;
    XpLight*     light;
};

struct XpSpotCuller
{
    const void** vtable;
    XpLight*     light;
    Frustum*     frustum;
};

// The culler slots' +0x0C query object is the Terrain's TerrainCuller (QuadPatch.h): the slots run
// its 0x100E6C70/0x100E6CB0 gathers and their 0x100E4370/0x100E4460 walks over its
// TerrainCullerNode tree.

} // anonymous namespace

// The renderer-owned light wrapper (0x38 bytes in the PE). The default constructor is the PE's
// zero fill; constructXpLight then writes the source fields.
struct XpLight
{
    XpLight()
        : releaseWarning(0), candidates(), source(0), active(0),
          cameraDistance(0.0f), category(0), culler(0)
    {
    }

    // The retail offsets. [RENDER] The shadow records at +0x00 and the policy byte at +0x29 that
    // gated them are deleted with the point/spot shadow maps.
    /* +0x10 */ unsigned char       releaseWarning;
    /* +0x14 */ std::vector<world::IObject*> candidates;
    /* +0x24 */ world::LightSource* source;
    /* +0x28 */ unsigned char       active;
    /* +0x2C */ float               cameraDistance;
    /* +0x30 */ int                 category;       // 0 dynamic, 1 static
    /* +0x34 */ XpCuller*           culler;

private:
    XpLight(const XpLight&);
    XpLight& operator=(const XpLight&);
};

// The culler slot forwards (PE 0x1018FAC0 / 0x1018FAE0), defined beside xpLightGatherTerrain.
bool xpLightCullerUnused(XpLight* light, void* argument);
void xpLightCullerDebug(XpLight* light);

namespace {

#if defined(_M_IX86)
static_assert(sizeof(XpCuller) == 0x08, "retail Point culler layout drift");
static_assert(sizeof(XpSpotCuller) == 0x0C, "retail Spot culler layout drift");
#endif

// PE 0x1018FAA0. Local accessor spellings are [INFERRED].
void setXpLightActive(XpLight* light, bool active)
{
    light->active = active;
}

// PE 0x1018FA20.
float xpLightCameraDistance(const XpLight* light)
{
    return light->cameraDistance;
}

// PE 0x1018FAB0.
std::vector<world::IObject*>& xpLightCandidates(XpLight* light)
{
    return light->candidates;
}

// The retail coordinator is the static object at PE 0x10259B20, built by a dynamic initializer
// (PE 0x101CD240 -> 0x100EF100) and destroyed at exit by an atexit thunk (PE 0x101D9240 ->
// 0x100EFBA0). The native object is the same static with the same constructor and destructor.
// [CHOSEN, not recovered] The retail coordinator heap-allocates its LightMerger (+0xB0), its two
// light collectors (+0xB4 static, 0x30 bytes; +0xB8 dynamic, 0x68 bytes) and its saved sun record
// (+0xC0, a LightMergerRecord); this aggregate holds them by value, flattened (the collectors are
// staticLights/staticFrame/staticFlags and dynamicLights/dynamicFrame). The members are declared so
// that the implicit member destruction after ~XpCoordinator runs in the retail delete order:
// dynamic collector, static collector, saved record, merger, then +0xA4, +0x98, +0x70, +0x48 and
// the registration table. The collectors' own constructors run on these members from the
// coordinator constructor (staticCollectorConstruct PE 0x101903F0, dynamicCollectorConstruct PE
// 0x10191050). [UNRESOLVED] The two vectors at retail +0x28 and +0x38 (built and freed
// by the constructor and destructor, no other user found) are not modelled. The owned-light set
// (+0x98), its creation flag (+0xC4), the two record lookups (+0x48 / +0x70) and the saved sun
// record exist only for the unreachable island helpers (PE 0x100EF380, 0x100EF700, 0x100EF960,
// 0x100EF9F0 and the lookups 0x100EEDA0 .. 0x100EFAA0); no reachable path fills them.
struct XpCoordinator
{
    XpCoordinator();
    ~XpCoordinator();

    std::map<world::LightSource*, XpLight*> bySource;   // retail +0x00, a stdext hash_map
    std::vector<XpLight*> iterationOrder;               // native: the hash_map's insertion order

    // [CHOSEN, not recovered] Unobservable-type exception. Retail +0x48 and +0x70 are two
    // stdext hash_maps (pointer key, pointer value; PE 0x100EF7F0 / 0x100EF7A0 are their
    // find-or-insert lookups, PE 0x100EF700 / 0x100EF960 their only writers, and none has a
    // reachable caller), so both are always empty. The value type is the pooled record pointer
    // that 0x100EF700 stores (from the LightMerger lookup 0x10194020) and 0x100EF9F0 hands to
    // 0x10193990; the key type is a pointer. Neither choice can be observed.
    typedef std::map<const void*, LightMergerRecord*> RecordLookup;
    RecordLookup recordLookupB;                         // +0x48
    RecordLookup recordLookupC;                         // +0x70
    // The owned-light set (retail coordinator +0x98, a std::set of light sources). Only the
    // unreachable PE 0x100EF380 fills it, so it stays empty in 1.50.
    std::set<world::LightSource*> ownedLights;
    std::multimap<void*, world::LightSource*> geometryPairs;   // +0xA4

    LightMerger merger;                                 // retail +0xB0 (heap)
    // Retail +0xC0 (heap) and the byte at +0xBC that says it holds the light manager's colours;
    // PE 0x100EF9F0 is their only user.
    LightMergerRecord savedSunRecord;
    bool sunSaved;

    // The static collector (retail +0xB4): light set +0x10, frame vector +0x1C.
    std::set<XpLight*> staticLights;
    std::vector<XpLight*> staticFrame;
    // The static collector's flags dword (retail collector +0x00): zero from construction (PE
    // 0x101903F0); its only writer, PE 0x10190150, is unreachable, so it stays zero in 1.50.
    unsigned int staticFlags;
    // The static collector's frame time (retail collector +0x08): stored by its update (PE
    // 0x1019046B) and handed to each light it prepares (PE 0x101901CE).
    double staticTime;

    // The dynamic collector (retail +0xB8). [CHOSEN, not recovered] Unobservable-type exception for
    // the three containers that never receive an element: retail +0x00 (a tree whose node keeps its
    // nil byte at +0x15, so an eight-byte value), +0x28 (a tree with a four-byte value) and +0x34 (a
    // stdext hash set: list +0x38, nine bucket iterators +0x44, mask/maxidx +0x54/+0x58). Their only
    // users are the constructor (PE 0x10191050), the clears (PE 0x10190D80, 0x10191120 and its
    // inlined copy at 0x10191181..0x101911B2) and the destructor; no instruction inserts into them,
    // so their key and value types cannot be observed (chosen: pointers).
    std::map<const void*, const void*> dynamicUnusedMap;   // +0x00
    std::set<XpLight*> dynamicLights;                      // +0x0C
    std::vector<XpLight*> dynamicFrame;                    // +0x18
    std::set<const void*> dynamicUnusedSet;                // +0x28
    std::unordered_set<const void*> dynamicUnusedHash;     // +0x34
    // The dynamic collector's frame time (retail +0x60): stored by its update (PE 0x101911BD) and
    // handed to each light it prepares (PE 0x10190E35).
    double dynamicTime;

    // The byte at +0xC4 that PE 0x100EF380 raises while it registers an owned light.
    unsigned char registeringOwnedLight;
};

XpCoordinator g_xpCoordinator;

inline LightManager* xpLightManager()
{
    return g_LightManager;
}

inline float squaredDistance(const Vec3& a, const Vec3& b)
{
    const float x = a.x - b.x;
    const float y = a.y - b.y;
    const float z = a.z - b.z;
    return x * x + y * y + z * z;
}

inline Vec3 matrixTranslation(const Mat4& matrix)
{
    Vec3 value;
    value.x = matrix.m[12];
    value.y = matrix.m[13];
    value.z = matrix.m[14];
    return value;
}

inline float xpEffectiveRadius(const XpLight* light)
{
    float radius = light->source->getVisualBoundingRadius();
    if (lightSourceTemplatePointer(light->source)->getIsDynamic())
        radius *= xpLightManager()->getDynamicLightRangeScale();
    return radius;
}

void appendCandidate(std::vector<world::IObject*>& destination, world::IObject* object)
{
    destination.push_back(object);
}

void pointQueryObjects(const Vec3& centre, float radius, std::vector<world::IObject*>& destination)
{
    std::vector<world::IObject*> hostObjects;
    reinterpret_cast<world::ObjectManager*>(objectManager)->getObjectsFromGrid(
        centre, radius, hostObjects, 0);
    for (std::vector<world::IObject*>::const_iterator it = hostObjects.begin();
         it != hostObjects.end(); ++it)
        appendCandidate(destination, *it);
}

void spotQueryObjects(const Frustum& frustum, std::vector<world::IObject*>& destination)
{
    std::vector<world::IObject*> hostObjects;
    reinterpret_cast<world::ObjectManager*>(objectManager)->getObjectsFromGrid(
        frustum, hostObjects, 0, false, 0.0f, 0.0f);
    for (std::vector<world::IObject*>::const_iterator it = hostObjects.begin();
         it != hostObjects.end(); ++it)
        appendCandidate(destination, *it);
}

bool localControlledObject(world::IObject*& controlled)
{
    world::IPlayer* const player =
        reinterpret_cast<world::PlayerManager*>(playerManager)->getLocalHumanPlayer();
    if (!player)
    {
        controlled = 0;
        return false;
    }
    controlled = player->getVehicle();
    return true;
}

// PE 0x101BCDF0, reached through the Point culler's first slot PE 0x101BCEA0. The controlled
// object is not null-checked, as in retail.
void pointCollect(XpCuller* culler, double, std::vector<world::IObject*>& destination, int)
{
    const Vec3& position = culler->light->source->getAbsolutePosition();
    const float radius = xpLightRadius(culler->light);
    pointQueryObjects(position, radius, destination);

    world::IObject* controlled = 0;
    if (!localControlledObject(controlled))
        return;

    const Vec3& sourcePosition = culler->light->source->getAbsolutePosition();
    const Vec3& controlledPosition = controlled->getAbsolutePosition();
    const float dx = controlledPosition.x - sourcePosition.x;
    const float dy = controlledPosition.y - sourcePosition.y;
    const float dz = controlledPosition.z - sourcePosition.z;
#if defined(__APPLE__)
    // [CHOSEN, not recovered] The root of the unstored sum is compared.
    const bf2x87::Real distance = bf2x87::squareRoot(bf2x87::wide(dx) * dx
        + bf2x87::wide(dz) * dz + bf2x87::wide(dy) * dy);
    if (bf2x87::less(distance, bf2x87::wide(radius)))
        appendCandidate(destination, controlled);
#else
    if (std::sqrt(dx * dx + dz * dz + dy * dy) < radius)
        appendCandidate(destination, controlled);
#endif
}

// PE 0x101BCF60.
void spotCollect(XpSpotCuller* culler, double, std::vector<world::IObject*>& destination, int)
{
    spotQueryObjects(*culler->frustum, destination);
}

// PE 0x101BCCA0. The position is fetched before the radius.
bool pointVisible(XpCuller* culler, const Frustum& camera)
{
    const Vec3& position = culler->light->source->getAbsolutePosition();
    const float radius = culler->light->source->getVisualBoundingRadius();
    return rendmath::frustumInside(camera, position, radius);          // PE 0x10079A60
}

// PE 0x101BD0C0. The spotlight Frustum's local box goes through its own transform (PE 0x100CE120)
// and the light is visible unless one camera plane rejects all eight corners. This is
// conservative intersection, not containment.
bool spotVisible(XpSpotCuller* culler, const Frustum& camera)
{
    const BoxCorners corners(culler->frustum->m_boundingBox,
                             culler->frustum->m_transformation);
    return !corners.outside(camera);                                   // sub_101C6C50
}

// PE 0x101BCD50: unreachable in 1.50: no code or data references. [INFERRED] name. Tests the Point
// culler's light against the current render view's camera frustum: the frustum is fetched first,
// then the light's range (LightSource slot +0x188) and absolute transformation (+0x80), and the
// position is the transformation's translation row.
bool pointInRenderViewFrustum(XpCuller* culler)
{
    Frustum* const camera = dx9rend::rendererGetRenderView()->getFrustumPtr();   // renderer +0x48, view +0x74
    world::LightSource* const source = culler->light->source;
    const float range = source->getAttenuationRange1();
    const Mat4& transform = source->getAbsoluteTransformation();
    const Vec3 position = { transform.m[12], transform.m[13], transform.m[14] };
    return rendmath::frustumInside(*camera, position, range);
}

// PE 0x101BCEE0: unreachable in 1.50: no code or data references. [INFERRED] name. Rebuilds the Spot
// culler's frustum from the light's current transformation and cone angle, as its constructor
// (PE 0x101BD000) does after allocating it.
void refreshSpotCullerFrustum(XpSpotCuller* culler)
{
    world::LightSource* const source = culler->light->source;
    const Mat4& transform = source->getAbsoluteTransformation();
    const float coneAngle = lightSourceTemplatePointer(source)->getConeAngle1();
    rendmath::frustumSetPerspective(*culler->frustum, coneAngle, 1.0f, 0.0f, 100.0f, transform);
}

// PE 0x101BD080: unreachable in 1.50: no code or data references. [INFERRED] name. The body of
// spotVisible (PE 0x101BD0C0) taking the spotlight Frustum itself instead of its culler.
bool spotFrustumVisible(const Frustum* spotFrustum, const Frustum& camera)
{
    const BoxCorners corners(spotFrustum->m_boundingBox, spotFrustum->m_transformation);
    return !corners.outside(camera);                                   // sub_101C6C50
}

void terrainQuerySphereWalk(TerrainCuller* query, TerrainCullerNode* node)
{
    if (node->childrenWalked)
    {
        for (unsigned int i = 0; i < 4; ++i)
        {
            TerrainCullerNode* const child = node->child[i];
            if (child->visible)
                terrainQuerySphereWalk(query, child);
        }
        return;
    }

    // The shipped leaf body asks the dirty byte twice; refresh clears it on the first call.
    if (node->bounds.dirty)
        node->bounds.refresh();
    const float nodeRadius = node->bounds.radius;
    if (node->bounds.dirty)
        node->bounds.refresh();

    const float totalRadius = nodeRadius + query->gatherRadius;
    const float* const centre = query->gatherCentre;
    const Vec3 gatherCentre = { centre[0], centre[1], centre[2] };
    if (totalRadius > std::sqrt(squaredDistance(gatherCentre, node->bounds.centre))
        && node->patch)
    {
        query->gathered.push_back(node);
    }
}

void terrainQueryFrustumWalk(TerrainCuller* query, TerrainCullerNode* node)
{
    if (node->childrenWalked)
    {
        for (unsigned int i = 0; i < 4; ++i)
        {
            TerrainCullerNode* const child = node->child[i];
            if (child->visible)
                terrainQueryFrustumWalk(query, child);
        }
        return;
    }

    if (node->bounds.dirty)
        node->bounds.refresh();
    const float nodeRadius = node->bounds.radius;
    if (node->bounds.dirty)
        node->bounds.refresh();
    if (rendmath::frustumInside(*query->frustum, node->bounds.centre, nodeRadius) && node->patch)
    {
        query->gathered.push_back(node);
    }
}

void terrainQuerySphere(TerrainCuller* query, const Vec3* centre, float radius)
{
    query->gathered.clear();
    query->gatherCentre = &centre->x;
    query->gatherRadius = radius;
    query->gatherRadiusSq = radius * radius;
    if (query->nodes->visible)
        terrainQuerySphereWalk(query, query->nodes);
}

void terrainQueryFrustum(TerrainCuller* query, Frustum* frustum)
{
    query->gathered.clear();
    query->frustum = frustum;
    if (query->nodes->visible)
        terrainQueryFrustumWalk(query, query->nodes);
}

// PE 0x101BCEA0: forwards the destination and flag to PE 0x101BCDF0.
void RD9_THUNK pointCollectSlot(void* self RD9_THUNK_EDX, double time,
                                 std::vector<world::IObject*>* destination, int flag)
{
    pointCollect(static_cast<XpCuller*>(self), time, *destination, flag);
}
void RD9_THUNK spotCollectSlot(void* self RD9_THUNK_EDX, double time,
                                std::vector<world::IObject*>* destination, int flag)
{
    spotCollect(static_cast<XpSpotCuller*>(self), time, *destination, flag);
}
bool RD9_THUNK pointVisibleSlot(void* self RD9_THUNK_EDX, const Frustum* camera)
{
    return pointVisible(static_cast<XpCuller*>(self), *camera);
}
bool RD9_THUNK spotVisibleSlot(void* self RD9_THUNK_EDX, const Frustum* camera)
{
    return spotVisible(static_cast<XpSpotCuller*>(self), *camera);
}
// PE 0x101BCCE0 / 0x101BCF50. Both retail +0x08 bodies are `xor al,al; ret 4`: one opaque stack argument is consumed even
// though the result is always false.  Keep that argument so an indirect call cannot unbalance esp.
bool RD9_THUNK cullerFalseSlot(void* RD9_THUNK_EDX, void*) { return false; }
// PE 0x101BCCF0. The position is fetched before the radius.
void RD9_THUNK pointSecondCullSlot(void* self RD9_THUNK_EDX, TerrainCuller* query)
{
    XpCuller* const culler = static_cast<XpCuller*>(self);
    const Vec3& position = culler->light->source->getAbsolutePosition();
    terrainQuerySphere(query, &position,
                       culler->light->source->getVisualBoundingRadius());
}
// PE 0x101BCF30.
void RD9_THUNK spotSecondCullSlot(void* self RD9_THUNK_EDX, TerrainCuller* query)
{
    terrainQueryFrustum(query, static_cast<XpSpotCuller*>(self)->frustum);
}
// PE 0x101BCD30.
void RD9_THUNK pointFillRecordSlot(void* RD9_THUNK_EDX, unsigned char* record)
{
    *reinterpret_cast<float*>(record + 0x3C) = 1.0f;
    *reinterpret_cast<float*>(record + 0x40) = 1.0f;
}
// PE 0x101BCFB0. Both cone angles are fetched before either cosine is taken.
void RD9_THUNK spotFillRecordSlot(void* self RD9_THUNK_EDX, unsigned char* record)
{
    XpSpotCuller* const culler = static_cast<XpSpotCuller*>(self);
    const float degreesToRadians = 0.01745329251994329577f;
    const float inner = culler->light->source->getConeAngle1();
    const float outer = culler->light->source->getConeAngle2();
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Each degree product feeds FCOS unstored.
    *reinterpret_cast<float*>(record + 0x3C) =
        bf2x87::store(bf2x87::cosValue(bf2x87::wide(inner) * degreesToRadians));
    *reinterpret_cast<float*>(record + 0x40) =
        bf2x87::store(bf2x87::cosValue(bf2x87::wide(outer) * degreesToRadians));
#else
    *reinterpret_cast<float*>(record + 0x3C) = std::cos(inner * degreesToRadians);
    *reinterpret_cast<float*>(record + 0x40) = std::cos(outer * degreesToRadians);
#endif
}
// PE 0x101BCDB0.
void RD9_THUNK pointDebugSlot(void* RD9_THUNK_EDX) {}
// PE 0x101BCF90. Draws the culler's +0x08 frustum with colour 0 and flag 0.
void RD9_THUNK spotDebugSlot(void* self RD9_THUNK_EDX)
{
    XpSpotCuller* const culler = static_cast<XpSpotCuller*>(self);
    static_cast<IDebugRenderer*>(g_DebugRenderer)->queueFrustum(*culler->frustum, 0, 0);
}

// PE 0x10222230.
const void* const kPointCullerVtable[6] =
{
    reinterpret_cast<void*>(&pointCollectSlot),
    reinterpret_cast<void*>(&pointVisibleSlot),
    reinterpret_cast<void*>(&cullerFalseSlot),
    reinterpret_cast<void*>(&pointSecondCullSlot),
    reinterpret_cast<void*>(&pointFillRecordSlot),
    reinterpret_cast<void*>(&pointDebugSlot)
};

// PE 0x10222268.
const void* const kSpotCullerVtable[6] =
{
    reinterpret_cast<void*>(&spotCollectSlot),
    reinterpret_cast<void*>(&spotVisibleSlot),
    reinterpret_cast<void*>(&cullerFalseSlot),
    reinterpret_cast<void*>(&spotSecondCullSlot),
    reinterpret_cast<void*>(&spotFillRecordSlot),
    reinterpret_cast<void*>(&spotDebugSlot)
};

// PE 0x101BCDC0.
XpCuller* constructPointCuller(XpCuller* culler, XpLight* light)
{
    culler->vtable = const_cast<const void**>(kPointCullerVtable);
    culler->light = light;
    return culler;
}

// PE 0x101BD000. The absolute transformation is fetched before the template cone angle.
XpSpotCuller* constructSpotCuller(XpSpotCuller* culler, XpLight* light)
{
    culler->vtable = const_cast<const void**>(kSpotCullerVtable);
    culler->light = light;
    void* const storage = ::renddx9abi::engineAlloc(sizeof(Frustum));
    culler->frustum = storage ? new (storage) Frustum : 0;
    world::LightSource* const source = light->source;
    const Mat4& transform = source->getAbsoluteTransformation();
    const float coneAngle = lightSourceTemplatePointer(source)->getConeAngle1();
    rendmath::frustumSetPerspective(*culler->frustum, coneAngle, 1.0f, 0.0f, 100.0f,
                                    transform);                       // PE 0x1007A460
    return culler;
}

// PE 0x1018FD80. Only Point (1) and Spot (2) templates receive a culler.
void createXpCuller(XpLight* light)
{
    const int type = lightSourceTemplatePointer(light->source)->getLightTypeEnum();
    if (type == 1)
    {
        void* const storage = ::renddx9abi::engineAlloc(sizeof(XpCuller));
        light->culler = storage
            ? constructPointCuller(static_cast<XpCuller*>(storage), light) : 0;
    }
    else if (type == 2)
    {
        void* const storage = ::renddx9abi::engineAlloc(sizeof(XpSpotCuller));
        light->culler = storage
            ? reinterpret_cast<XpCuller*>(
                  constructSpotCuller(static_cast<XpSpotCuller*>(storage), light))
            : 0;
    }
}

// PE 0x1018FAF0: the category from the source template's dynamic flag (dynamic 0, static 1).
// Unreachable in 1.50: no code or data references; [INFERRED] name. The constructor tail below
// carries the same first step.
void setXpLightCategory(XpLight* light)
{
    const world::LightSourceTemplate* const sourceTemplate =
        lightSourceTemplatePointer(light->source);
    light->category = sourceTemplate->getIsDynamic() ? 0 : 1;
}

// PE 0x1018FDE0 (also 0x1018FEA0): the tail the XpLight constructor (PE 0x1018FF40) shares with
// two standalone copies - the category from the template's dynamic flag (dynamic 0, static 1), the
// culler, then the shadow-record policy ([RENDER] deleted with the point/spot shadow maps).
// Unreachable in 1.50: no code or data references.
void refreshXpLightCategory(XpLight* light)
{
    setXpLightCategory(light);

    createXpCuller(light);
}

// PE 0x1018FF40: the XpLight constructor. It publishes the initial category before the source's
// addRef callback.
void constructXpLight(XpLight* light, world::LightSource* source)
{
    ::new (light) XpLight();
    light->source = source;
    light->cameraDistance = 1000000.0f; // exact 0x49742400 constructor constant
    light->category = 2;
    source->addRef();
    refreshXpLightCategory(light);
}

// PE 0x1018FFB0 (release source, raw-free culler, warning, candidate and record storage)
void destroyXpLight(XpLight* light)
{
    // Shipped order is significant. In particular, the Spot adapter is raw-freed without running
    // its destructor, leaking the heap Frustum at +8; reproduce that defect.
    light->source->release();
    ::renddx9abi::engineFree(light->culler);
    if (light->releaseWarning)                    // byte +0x10: the shipped warning
        ::renddx9abi::logf("XpLight::~XpLight: Need someway of releasing the lightsource.");
    // The candidates, then the records - the members' reverse declaration order.
    light->~XpLight();
}

// PE 0x10190EA0: the dynamic collector (coordinator +0xB8) inserts a light only when absent.
void dynamicCollectorRegister(XpLight* light)
{
    g_xpCoordinator.dynamicLights.insert(light);
}

// PE 0x101901F0: the static collector (coordinator +0xB4) inserts a light that is not flagged
// (source flag bit 0) or is enabled, then switches the source off (slot +0x1C4).
void staticCollectorRegister(XpLight* light)
{
    if ((lightSourceFlags(light->source) & 1u) == 0
        || light->source->getEnabled())
    {
        g_xpCoordinator.staticLights.insert(light);
        light->source->setEnabled(false);
    }
}

// PE 0x10190F00: the dynamic collector erases a light when present (equal_range, erase).
void dynamicCollectorUnregister(XpLight* light)
{
    g_xpCoordinator.dynamicLights.erase(light);
}

// PE 0x10190240: the static collector erases a light when present (equal_range, erase).
void staticCollectorUnregister(XpLight* light)
{
    g_xpCoordinator.staticLights.erase(light);
}

// PE 0x100EF050, collector dispatch; registration publication is in lmRegisterXpLight.
void registerWithCollector(XpLight* light)
{
    if (light->category == 0)
    {
        dynamicCollectorRegister(light);
        return;
    }
    if (light->category == 1)
    {
        staticCollectorRegister(light);
        return;
    }

    const std::string expression("");
    const std::string file("C:\\dice\\Projects\\BF2Branches\\Patch_1_50\\Code\\BF2\\Geom\\XpLightManager.cpp");
    ::renddx9abi::Debug(DebugTypeError, file, 0x227, expression);
}

// PE 0x100EEC00.
void unregisterFromCollector(XpLight* light)
{
    if (light->category == 0)
        dynamicCollectorUnregister(light);
    else if (light->category == 1)
        staticCollectorUnregister(light);
    else
    {
        const std::string expression("");
        const std::string file("C:\\dice\\Projects\\BF2Branches\\Patch_1_50\\Code\\BF2\\Geom\\XpLightManager.cpp");
        ::renddx9abi::Debug(DebugTypeError, file, 0x253, expression);
    }
}

// PE 0x100EED60: retail counts the registration table's equal range for the key (stdext hash_map
// equal_range, PE 0x100F0400) and answers whether it is nonempty; the native source-key map
// answers the same question.
bool xpSourceIsRegistered(world::LightSource* source)
{
    return g_xpCoordinator.bySource.count(source) != 0;
}

// PE 0x100EEE20: retail erases the registration node whose mapped value is the wrapper (PE
// 0x100F09D0 on the node found by a linear walk of the hash list), then removes the wrapper from its
// category collector. The collector removal runs whether or not a node was found.
void xpEraseRegistration(XpLight* light)
{
    for (std::map<world::LightSource*, XpLight*>::iterator it = g_xpCoordinator.bySource.begin();
         it != g_xpCoordinator.bySource.end(); ++it)
    {
        if (it->second == light)
        {
            g_xpCoordinator.bySource.erase(it);
            break;
        }
    }
    unregisterFromCollector(light);
}

// PE 0x100EF2B0, reached by manager +0x94 for Point/Spot lights. Eligibility, duplicate handling,
// 0x38-byte construction, collector classification and map publication preserve shipped order.
void lmRegisterXpLight(world::LightSource* light)
{
    const world::LightSourceTemplate* const objectTemplate =
        lightSourceTemplatePointer(light);

    if (!objectTemplate->getEnabled() && !objectTemplate->getIsDynamic())
        return;

    const int firstType = objectTemplate->getLightTypeEnum();
    if (firstType != 1 && objectTemplate->getLightTypeEnum() != 2)
        return;

    if ((objectTemplate->getFlags() & 0x800u) == 0 && !objectTemplate->getIsDynamic())
        return;

    if (!dx9rend::rendererIsExpansionPack() && !objectTemplate->getIsDynamic())
        return;

    // Reject a source whose registration count is nonzero.
    if (xpSourceIsRegistered(light))
        return;

    XpLight* const xp = static_cast<XpLight*>(::renddx9abi::engineAlloc(sizeof(XpLight)));
    constructXpLight(xp, light);
    registerWithCollector(xp);
    g_xpCoordinator.bySource.insert(std::make_pair(light, xp));
    g_xpCoordinator.iterationOrder.push_back(xp);
}

// PE 0x100EF940 / 0x100EEE90: source lookup, then removal of the registered wrapper.
void lmRemoveXpLight(world::LightSource* light)
{
    std::map<world::LightSource*, XpLight*>::iterator found =
        g_xpCoordinator.bySource.find(light);
    if (found == g_xpCoordinator.bySource.end())
        return;

    XpLight* const xp = found->second;
    // Erase the registration, then remove the wrapper from its category collector.
    xpEraseRegistration(xp);
    std::vector<XpLight*>::iterator ordered =
        std::find(g_xpCoordinator.iterationOrder.begin(),
                  g_xpCoordinator.iterationOrder.end(), xp);
    if (ordered != g_xpCoordinator.iterationOrder.end())
        g_xpCoordinator.iterationOrder.erase(ordered);
    destroyXpLight(xp);
    ::renddx9abi::engineFree(xp);
}

// PE 0x1018FD00: the distance from the render view's camera to the source, stored at +0x2C.
// Differences are source - camera, summed as (z*z + x*x) + y*y (fld z; fmul; fld x; fmul; faddp;
// fld y; fmul; faddp) before the square root. updateXpLight carries the same body inline.
void updateXpLightDistance(XpLight* light)
{
    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderView();
    const Vec3 camera = matrixTranslation(*view->getViewMatrixPtr());
    const Vec3 source = matrixTranslation(
        light->source->getAbsoluteTransformation());
    const float dx = source.x - camera.x;
    const float dy = source.y - camera.y;
    const float dz = source.z - camera.z;
    const float sum = (dz * dz + dx * dx) + dy * dy;
    light->cameraDistance = std::sqrt(sum);
}

// PE 0x1018FE10
void updateXpLight(XpLight* light)
{
    // PE 0x1018FE10 deliberately exempts the shipped default class value (1) from the disabled
    // source early-out. Static XpLights are therefore still considered after their registration
    // path invokes source->setEnabled(false).
    if ((lightSourceFlags(light->source) & 1u) != 0
        && !light->source->getEnabled()
        && light->category != 1)
    {
        setXpLightActive(light, false);
        return;
    }

    bool visible = true;
    if (dx9rend::g_rendererBase)
    {
        dx9rend::IRenderView* const view = dx9rend::rendererGetRenderView();
        updateXpLightDistance(light);
        // PE 0x1018FE47: fcomp against 100 (global 0x1023A6F8); only an ordered "greater" clears the
        // flag, so a NaN distance stays visible.
        visible = !(light->cameraDistance > 100.0f);
        if (visible)
        {
            const Frustum& cameraFrustum = *view->getFrustumPtr();
            const int type = lightSourceTemplatePointer(light->source)->getLightTypeEnum();
            if (type == 1)
                visible = pointVisible(light->culler, cameraFrustum);
            else if (type == 2)
                visible = spotVisible(reinterpret_cast<XpSpotCuller*>(light->culler),
                                      cameraFrustum);
        }
    }

    // PE BSS byte 0x102637E0 can force an otherwise rejected light active. Its shipped default is
    // false and no recovered console/property writer has yet been identified.
    static const bool forceVisible = false;
    if (!visible && forceVisible)
        visible = true;
    setXpLightActive(light, visible);
}

// PE 0x101900B0: empties the candidate list (the pointer-vector clear, PE 0x10032330 with
// 0, 0); retail also emptied the shadow records ([RENDER] deleted). prepareXpCandidates
// carries the same step inline. Unreachable in 1.50: no code or data references.
void clearXpCandidates(XpLight* light)
{
    light->candidates.clear();
}

// PE 0x101900E0
void prepareXpCandidates(XpLight* light, double time, int flag)
{
    clearXpCandidates(light);
    if (light->active)
    {
        const int type = lightSourceTemplatePointer(light->source)->getLightTypeEnum();
        if (type == 1)
            pointCollect(light->culler, time, light->candidates, flag);
        else if (type == 2)
            spotCollect(reinterpret_cast<XpSpotCuller*>(light->culler),
                        time, light->candidates, flag);
    }
    if (light->candidates.size() == 0)
        setXpLightActive(light, false);
}

// PE 0x100EEAA0 (fastcall lhs in ECX, rhs in EDX; lhs distance saved as a float first)
bool nearerXpLight(const XpLight* lhs, const XpLight* rhs)
{
    return xpLightCameraDistance(lhs) < xpLightCameraDistance(rhs);
}

// PE 0x101903F0: the static collector's constructor (0x30 bytes, built at PE 0x100EF248): frame
// time 0.0, flags 0, then the empty light set and the zeroed frame vector, which the aggregate's
// members already are.
void staticCollectorConstruct(XpCoordinator& coordinator)
{
    coordinator.staticTime = 0.0;
    coordinator.staticFlags = 0;
}

// PE 0x10191050: the dynamic collector's constructor (0x68 bytes, built at PE 0x100EF267): the
// +0x00 tree, the light set, the zeroed frame vector, the +0x28 tree and the +0x34 hash set (one
// bucket mask) start empty, as the aggregate's members already are; then the frame time 0.0.
void dynamicCollectorConstruct(XpCoordinator& coordinator)
{
    coordinator.dynamicTime = 0.0;
}

// PE 0x10190D80: empties the dynamic collector's +0x00 tree, then its +0x28 tree (each erase the
// nodes, relink the head, size 0). Unreachable in 1.50: no code or data reference; both are always
// empty.
void dynamicCollectorClearUnused()
{
    g_xpCoordinator.dynamicUnusedMap.clear();
    g_xpCoordinator.dynamicUnusedSet.clear();
}

// PE 0x101902F0: empties the static collector's frame vector (resize(0), PE 0x10101170).
void staticCollectorClearFrame()
{
    g_xpCoordinator.staticFrame.clear();
}

// PE 0x10190300: the static collector walks its light set in key order; an active light joins the
// frame (and, under flag bit 0x10, its culler's debug slot runs), and flag bit 0x1 runs the debug
// slot for every light.
void staticCollectorGather()
{
    const unsigned int flags = g_xpCoordinator.staticFlags;
    for (std::set<XpLight*>::const_iterator it = g_xpCoordinator.staticLights.begin();
         it != g_xpCoordinator.staticLights.end(); ++it)
    {
        XpLight* const light = *it;
        if (light->active)
        {
            g_xpCoordinator.staticFrame.push_back(light);
            if (flags & 0x10u)
                xpLightCullerDebug(light);
        }
        if (flags & 0x1u)
            xpLightCullerDebug(light);
    }
}

// PE 0x10190170: pops the frame's tail down to the budget (default 3, PE 0x1023A750) without
// deactivating the dropped lights, then prepares every remaining light with the collector's time
// (+0x08; debug slot first under flag bit 0x10).
void staticCollectorBudget()
{
    while (g_xpCoordinator.staticFrame.size() > 3u)
        g_xpCoordinator.staticFrame.pop_back();
    for (std::vector<XpLight*>::iterator it = g_xpCoordinator.staticFrame.begin();
         it != g_xpCoordinator.staticFrame.end(); ++it)
    {
        if (g_xpCoordinator.staticFlags & 0x10u)
            xpLightCullerDebug(*it);
        prepareXpCandidates(*it, g_xpCoordinator.staticTime, 1);
    }
}

// PE 0x10190460 (frame: store time, gather PE 0x10190300 of the active lights, sort by camera distance
// with PE 0x100EEAA0, budget PE 0x10190170 which only pops the tail without deactivating, then prepare).
void updateStaticCollector(double time)
{
    g_xpCoordinator.staticTime = time;
    staticCollectorClearFrame();
    staticCollectorGather();
    std::sort(g_xpCoordinator.staticFrame.begin(), g_xpCoordinator.staticFrame.end(),
              nearerXpLight);
    staticCollectorBudget();
}

// PE 0x10191120: the dynamic collector empties its frame vector (resize(0), PE 0x10101170), then
// the +0x28 tree and the +0x34 hash set (PE 0x10191900), both always empty (see the struct).
void dynamicCollectorClearFrame()
{
    g_xpCoordinator.dynamicFrame.clear();
    g_xpCoordinator.dynamicUnusedSet.clear();
    g_xpCoordinator.dynamicUnusedHash.clear();
}

// PE 0x10190DE0: trims the dynamic frame to the budget (default 3, PE 0x1023A7AC), deactivating
// every dropped light (PE 0x1018FAA0), then prepares the remaining lights with the collector's
// time (+0x60).
void dynamicCollectorTrimAndPrepare()
{
    while (g_xpCoordinator.dynamicFrame.size() > 3u)
    {
        setXpLightActive(g_xpCoordinator.dynamicFrame.back(), false);
        g_xpCoordinator.dynamicFrame.pop_back();
    }
    for (std::vector<XpLight*>::iterator it = g_xpCoordinator.dynamicFrame.begin();
         it != g_xpCoordinator.dynamicFrame.end(); ++it)
        prepareXpCandidates(*it, g_xpCoordinator.dynamicTime, 1);
}

// PE 0x10191170 (frame update: the inlined frame clear, then the time stored at +0x60; its
// trim-and-prepare tail is dynamicCollectorTrimAndPrepare).
void updateDynamicCollector(double time)
{
    dynamicCollectorClearFrame();
    g_xpCoordinator.dynamicTime = time;
    for (std::set<XpLight*>::const_iterator it = g_xpCoordinator.dynamicLights.begin();
         it != g_xpCoordinator.dynamicLights.end(); ++it)
    {
        if ((*it)->active)
            g_xpCoordinator.dynamicFrame.push_back(*it);
    }
    std::sort(g_xpCoordinator.dynamicFrame.begin(), g_xpCoordinator.dynamicFrame.end(),
              nearerXpLight);
    dynamicCollectorTrimAndPrepare();
}

void* candidateGeometry(world::IObject* candidate)
{
    return world::object_detail::LayoutAccess::geometry(candidate);
}


// Geometry instance dispatch uses fixed primary-table offsets until a shared typed view exists.
// All supported mesh classes must keep identical arity at these callee-cleanup slots.

inline void* geometrySlot(void* geometry, unsigned int offset)
{
    return (*reinterpret_cast<void***>(geometry))[offset / 4];
}

inline const LocalBoundingBox& geometryLocalBounds(void* geometry)
{
    typedef const void* (__thiscall* GetLocalBoundsFn)(void*);
    return *static_cast<const LocalBoundingBox*>(
        reinterpret_cast<GetLocalBoundsFn>(geometrySlot(geometry, 0x40))(geometry));
}

inline const Mat4& candidateAbsoluteTransformation(world::IObject* candidate)
{
    return candidate->getAbsoluteTransformation();
}

// PE 0x100EED10.
void updateXpCoordinator(double time)
{
    for (std::vector<XpLight*>::iterator it = g_xpCoordinator.iterationOrder.begin();
         it != g_xpCoordinator.iterationOrder.end(); ++it)
        updateXpLight(*it);
    updateStaticCollector(time);
    updateDynamicCollector(time);
}

// PE 0x100EEFC0.
void buildXpGeometryPairs()
{
    for (std::vector<XpLight*>::const_iterator it = g_xpCoordinator.iterationOrder.begin();
         it != g_xpCoordinator.iterationOrder.end(); ++it)
    {
        XpLight* const light = *it;
        if (!light->active)
            continue;
        world::IObject* const* const begin = light->candidates.data();
        const unsigned int count = static_cast<unsigned int>(light->candidates.size());
        for (unsigned int i = 0; i < count; ++i)
            g_xpCoordinator.geometryPairs.insert(
                std::make_pair(candidateGeometry(begin[i]), light->source));
    }
}

// PE 0x100F3FB0.
void destroyLightManager(void* self)
{
    LightManagerData& d = lm(self);
    // Restore all three most-derived tables before touching any owned state.
    d.primaryVtable = kPrimaryVtable;
    d.renderPathVtable = kRenderPathVtable;

    if (d.simpleShadowMap)
        ::renddx9abi::engineFree(d.simpleShadowMap);

    dx9rend::textureReleaseRef(d.normalCubeTexture);
    dx9rend::textureReleaseRef(d.whiteTexture);
    dx9rend::textureReleaseRef(d.blackTexture);
    dx9rend::textureReleaseRef(d.framedTexture);
    dx9rend::textureReleaseRef(d.blackCubeTexture);
    dx9rend::textureReleaseRef(d.conicalAttenuation);
    dx9rend::textureReleaseRef(d.radialAttenuation);

    // The PE's release order over the parameter handles (+0x8B4 down, then the shuffled tail).
    static const LightShaderParameter parameterOrder[] =
    {
        kParamTexLayer6, kParamTexLayer5, kParamTexLayer4, kParamTexLayer3, kParamTexLayer2,
        kParamTexLayer1, kParamTexLayer0, kParamLightMapIntensityBias, kParamShadowIntensityBias,
        kParamEyePos, kParamEyeDof, kParamViewportMap, kParamViewportMap2, kParamProjectorMask,
        kParamLightOccluderViewProj, kParamLightViewProj, kParamWorldViewProj, kParamViewProj,
        kParamLightAttenuationRangeInv, kParamLightAttenuationRange, kParamLightCol,
        kParamLightWorldPos, kParamLightPos, kParamLightDir, kParamParaboloidZValues,
        kParamStencilRef, kParamStencilFunc, kParamStencilPass
    };
    for (unsigned int i = 0; i < sizeof(parameterOrder) / sizeof(parameterOrder[0]); ++i)
    {
        dx9rend::ShaderParameter*& parameter = d.shaderParameters[parameterOrder[i]];
        if (parameter)
            parameter->release();
        parameter = 0;
    }
    dx9rend::Effect*& effect = d.shadowEffect;
    if (effect)
        dx9rend::effectRelease(effect);
    effect = 0;

    for (unsigned int i = 0; i < 4; ++i)
        dx9rend::textureReleaseRef(d.specularLuts[i]);
    d.currentSpecularLut = 0;

    if (d.skyLight)
        d.skyLight->release();

    IUnknown** const objects[] =
    {
        reinterpret_cast<IUnknown**>(&d.boxVertices), reinterpret_cast<IUnknown**>(&d.boxIndices),
        reinterpret_cast<IUnknown**>(&d.sphereVertices), reinterpret_cast<IUnknown**>(&d.sphereIndices),
        reinterpret_cast<IUnknown**>(&d.cylinderVertices),
        reinterpret_cast<IUnknown**>(&d.cylinderIndices)
    };
    for (unsigned int i = 0; i < sizeof(objects) / sizeof(objects[0]); ++i)
    {
        IUnknown*& object = *objects[i];
        if (object)
            object->Release();
        object = 0;
    }

    d.device->Release();
    dx9rend::unregisterRenderPathListener(
        reinterpret_cast<dx9rend::RenderPathListener*>(&d.renderPathVtable));

    // The containers, in the PE's order - +0x970 down to +0x180, the members' reverse
    // declaration order.
    d.simpleShadowIOBitCount = 0;
    d.~LightManagerData();
}

int RD9_THUNK lm_addRef(void* self RD9_THUNK_EDX)
{
    return ++lm(self).refCount;
}

int RD9_THUNK lm_getRef(void* self RD9_THUNK_EDX)
{
    return lm(self).refCount;
}

// PE 0x100F7A50
int RD9_THUNK lm_release(void* self RD9_THUNK_EDX)
{
    const int remaining = --lm(self).refCount;
    if (remaining == 0)
    {
        destroyLightManager(self);
        ::renddx9abi::engineFree(self);
    }
    return remaining;
}

// PE 0x100F78E0
void* RD9_THUNK lm_queryInterface(void* self RD9_THUNK_EDX, unsigned int iid)
{
    if (iid == RD9_IID_IBASE || iid == IID_ILightManager)
        return self;
    return 0;
}

// LightManager::init calls this on every invocation, even after the sunlight at
// +0x840 has already been created.  D3DX return values are deliberately ignored: retail proceeds
// directly through every returned mesh pointer and therefore has no failure recovery arm here.
// PE 0x100F17A0.
void buildLightDebugGeometry(void* self)
{
    IDirect3DDevice9* const device = lm(self).device;

    ID3DXMesh* sphereSource = 0;
    ID3DXMesh* sphere = 0;
    D3DXCreateSphere(device, 1.0f, 20, 20, &sphereSource, 0);
    sphereSource->CloneMeshFVF(0x660u, D3DFVF_XYZ, device, &sphere);

    ID3DXMesh* cylinderSource = 0;
    ID3DXMesh* cylinder = 0;
    D3DXCreateCylinder(device, 0.0f, 1.0f, 1.0f, 20, 20, &cylinderSource, 0);
    cylinderSource->CloneMeshFVF(0x660u, D3DFVF_XYZ, device, &cylinder);

    ID3DXMesh* boxSource = 0;
    ID3DXMesh* box = 0;
    D3DXCreateBox(device, 1.0f, 1.0f, 1.0f, &boxSource, 0);
    boxSource->CloneMeshFVF(0x660u, D3DFVF_XYZ, device, &box);

    if (lm(self).boxVertices)
        lm(self).boxVertices->Release();
    if (lm(self).boxIndices)
        lm(self).boxIndices->Release();
    if (lm(self).sphereVertices)
        lm(self).sphereVertices->Release();
    if (lm(self).sphereIndices)
        lm(self).sphereIndices->Release();
    if (lm(self).cylinderVertices)
        lm(self).cylinderVertices->Release();
    if (lm(self).cylinderIndices)
        lm(self).cylinderIndices->Release();

    box->GetVertexBuffer(&lm(self).boxVertices);
    box->GetIndexBuffer(&lm(self).boxIndices);
    cylinder->GetVertexBuffer(&lm(self).cylinderVertices);
    cylinder->GetIndexBuffer(&lm(self).cylinderIndices);
    sphere->GetVertexBuffer(&lm(self).sphereVertices);
    sphere->GetIndexBuffer(&lm(self).sphereIndices);

    lm(self).boxVertexCount = box->GetNumVertices();
    lm(self).boxFaceCount = box->GetNumFaces();
    lm(self).cylinderVertexCount = cylinder->GetNumVertices();
    lm(self).cylinderFaceCount = cylinder->GetNumFaces();
    lm(self).sphereVertexCount = sphere->GetNumVertices();
    lm(self).sphereFaceCount = sphere->GetNumFaces();

    boxSource->Release();
    box->Release();
    cylinderSource->Release();
    cylinder->Release();
    sphereSource->Release();
    sphere->Release();
}

// The vector<bool> at +0x17C (bit count) / +0x180 (words). New words are zeroed; shrinking keeps the
// storage and clears the spare bits of the last word.
class SimpleShadowMapIOStorage
{
public:
    explicit SimpleShadowMapIOStorage(void* manager)
        : count(lm(manager).simpleShadowIOBitCount),
          words(lm(manager).simpleShadowIOWords) {}

    void set(unsigned int index, bool value)
    {
        unsigned int& word = words.data()[index >> 5];
        const unsigned int mask = 1u << (index & 31u);
        if (value) word |= mask;
        else word &= ~mask;
    }

    void resize(unsigned int size, bool value)
    {
        const unsigned int oldSize = count;
        if (size == oldSize)
            return;
        const unsigned int wordCount = (size + 31u) >> 5;
        words.resize(wordCount, 0u);
        count = size;
        if (size > oldSize)
        {
            for (unsigned int i = oldSize; i < size; ++i) set(i, value);
        }
        else if (size & 31u)
        {
            words.data()[wordCount - 1u] &= (1u << (size & 31u)) - 1u;
        }
    }

    void fill(bool value)
    {
        // PE 100AED90 fills only [begin,end), leaving the unused high bits alone.
        for (unsigned int i = 0; i < count; ++i) set(i, value);
    }

private:
    SimpleShadowMapIOStorage(const SimpleShadowMapIOStorage&);
    SimpleShadowMapIOStorage& operator=(const SimpleShadowMapIOStorage&);

    unsigned int& count;
    std::vector<unsigned int>& words;
};

const renddx9abi::HostString& simpleShadowStringSetting(
    const renddx9abi::HostString& key, const renddx9abi::HostString& fallback)
{
    return settings->stringGet(key, fallback);
}

renddx9abi::HostString simpleShadowLevelPath()
{
    const renddx9abi::HostString levelDefault("");
    const renddx9abi::HostString levelKey("GSLoadLevel");
    const renddx9abi::HostString directoryDefault("");
    const renddx9abi::HostString directoryKey("GSLevelDirectory");
    const renddx9abi::HostString& level = simpleShadowStringSetting(levelKey, levelDefault);
    const renddx9abi::HostString& directory = simpleShadowStringSetting(directoryKey, directoryDefault);
    renddx9abi::HostString path(directory);
    path += level;
    return path;
}

void loadSimpleShadowMapsBody(void* self)
{
    if (lm(self).simpleShadowMap)
        ::renddx9abi::engineFree(lm(self).simpleShadowMap);

    world::IHeightmapCluster* const cluster = static_cast<world::IHeightmapCluster*>(
        static_cast<void*>(classManager->getSingleton(renddx9abi::HostString("HeightmapCluster"))));
    // IHeightmapCluster PE +0x2C / ELF +0x30 is getHeightmapSize, not getWorldSize.
    const int size = cluster->getHeightmapSize() / 2;
    lm(self).simpleShadowIOColumnOrigin = (size + 8) / 2;
    const int rowOrigin = (size + 2) / 2;
    lm(self).simpleShadowIORowOrigin = rowOrigin;
    lm(self).simpleShadowIOStride = size + 8;
    lm(self).simpleShadowStride = (size + 8) / 4;
    lm(self).simpleShadowRowCount = size + 2;
    lm(self).simpleShadowLimit = static_cast<float>(size);
    lm(self).simpleShadowColumnOrigin = ((size + 8) / 4) / 2;
    lm(self).simpleShadowRowOrigin = rowOrigin;
    const unsigned int packedBytes = lm(self).simpleShadowRowCount * lm(self).simpleShadowStride;
    lm(self).simpleShadowMap =
        static_cast<unsigned char*>(::renddx9abi::engineAlloc(packedBytes));
    memset(lm(self).simpleShadowMap, 0xFF, packedBytes);

    const renddx9abi::HostString levelPath = simpleShadowLevelPath();
    renddx9abi::HostStream* stream;
    {
        renddx9abi::HostString path(levelPath);
        path += "/SimpleShadowmap.raw";
        stream = fileManager->open(path, static_cast<renddx9abi::HostFileMode>(2),
                                   static_cast<renddx9abi::HostFileAccess>(0), 0);
    }
    // Retail's missing-main-file return leaves the previous IO bit vector untouched. In
    // particular, the first absent main file does not manufacture IO storage for the sampler.
    if (!stream) return;

    const unsigned int imageBytes = size * size;
    unsigned char* source = static_cast<unsigned char*>(::renddx9abi::engineAlloc(imageBytes));
    stream->read(source, imageBytes);
    stream->release();
    for (int y = 0; y < size; ++y)
    {
        const unsigned char* row = source + (size - 1 - y) * size;
        for (int x = 0; x < size; ++x)
        {
            // PE uses x87 precision up to floor(double): do not round the product to float.
            // The multiplier's exact stored float is 0x3C40C0C1, i.e. float(3/255).
            const unsigned int value = static_cast<unsigned int>(std::floor(
                static_cast<double>(row[x]) * static_cast<double>(3.0f / 255.0f) + 0.5));
            const int packedX = x + 4;
            const unsigned int shift = 2u * (packedX & 3);
            unsigned char& destination = lm(self).simpleShadowMap[
                (y + 1) * lm(self).simpleShadowStride + (packedX >> 2)];
            destination = static_cast<unsigned char>(
                (destination & ~(3u << shift)) | ((value & 3u) << shift));
        }
    }
    ::renddx9abi::engineFree(source);

    SimpleShadowMapIOStorage io(self);
    io.resize(lm(self).simpleShadowIOStride * lm(self).simpleShadowRowCount, false);
    io.fill(true);
    {
        renddx9abi::HostString path(levelPath);
        path += "/SimpleShadowmapIO.raw";
        stream = fileManager->open(path, static_cast<renddx9abi::HostFileMode>(2),
                                   static_cast<renddx9abi::HostFileAccess>(0), 0);
    }
    if (!stream) return;

    source = static_cast<unsigned char*>(::renddx9abi::engineAlloc(imageBytes));
    stream->read(source, imageBytes);
    stream->release();
    for (int y = 0; y < size; ++y)
    {
        const unsigned char* row = source + (size - 1 - y) * size;
        for (int x = 0; x < size; ++x)
            io.set((y + 1) * lm(self).simpleShadowIOStride + x + 4, row[x] > 127);
    }
    ::renddx9abi::engineFree(source);
}

// PE 0x100F1F60, reached by the zero-argument `Lightmanager.init` console object.  The debug mesh
// rebuild above is unconditional; only creation of the special sunlight is guarded by +0x840.
void initializeLightManagerBody(void* self)
{
    buildLightDebugGeometry(self);
    if (lm(self).sunLight)
        return;

    world::ObjectTemplateManager* const templateManager =
        static_cast<world::ObjectTemplateManager*>(classManager->getSingleton(
            renddx9abi::HostString("ObjectTemplateManager")));
    if (!templateManager)
        return;

    const renddx9abi::HostString sunlightName("sunlight");
    world::LightSourceTemplate* objectTemplate =
        static_cast<world::LightSourceTemplate*>(
            templateManager->getTemplate(hostStdString(sunlightName), 38087u));
    if (!objectTemplate)
    {
        // PE constructs the object name before the class name and therefore destroys the class
        // string first after createTemplate returns.
        const renddx9abi::HostString objectName("sunlight");
        const renddx9abi::HostString className("LightSource");
        objectTemplate = static_cast<world::LightSourceTemplate*>(
            templateManager->createTemplate(
                hostStdString(className), hostStdString(objectName)));
    }
    if (!objectTemplate)
        return;

    {
        const renddx9abi::HostString directional("Directional");
        objectTemplate->setLightType(hostStdString(directional));
    }
    objectTemplate->setCastsDynamicShadow(false);
    objectTemplate->setCastsStaticShadow(true);
    objectTemplate->setFov(8.0f);
    objectTemplate->setSupportsPerPixel(true);

    world::LightSource* const light =
        static_cast<world::LightSource*>(objectTemplate->createObject());
    lm(self).sunLight = light;
    light->init();
    {
        const renddx9abi::HostString name("sun");
        light->setName(hostStdString(name));
    }
    light->setAsSkyLight();
    const Vec3 color = { 0.8f, 0.8f, 0.8f };
    light->setColor(color);
    const Vec3 direction = { 0.0f, -0.7f, -0.7f };
    light->setDirection(direction);
}


// Tests required and forbidden light properties, including the allowed light-type mask.
// Requires a light with a template. Typed calls account for the two-slot PE/ELF
// difference in the light-template interface.

// PE 0x100F14B0.
bool lightMatchesMask(world::LightSource* light, unsigned int required, unsigned int forbidden)
{
    const world::LightSourceTemplate* const t = lightSourceTemplatePointer(light);

    if ((required  & 0x1u) && !light->getEnabled()) return false;
    if ((forbidden & 0x1u) &&  light->getEnabled()) return false;
    if ((required  & 0x2u) &&  light->getEnabled()) return false;
    if ((forbidden & 0x2u) && !light->getEnabled()) return false;

    if ((required  & 0x8u) && !t->getIsDynamic()) return false;
    if ((forbidden & 0x8u) &&  t->getIsDynamic()) return false;
    if ((required  & 0x4u) &&  t->getIsDynamic()) return false;
    if ((forbidden & 0x4u) && !t->getIsDynamic()) return false;

    if ((required  & 0x20u) && !t->getCastsDynamicShadow()) return false;
    if ((forbidden & 0x20u) &&  t->getCastsDynamicShadow()) return false;
    if ((required  & 0x10u) && !t->getCastsStaticShadow()) return false;
    if ((forbidden & 0x10u) &&  t->getCastsStaticShadow()) return false;

    if ((required  & 0x80u) && !t->getSelfLights()) return false;
    if ((forbidden & 0x80u) &&  t->getSelfLights()) return false;
    if ((required  & 0x40u) && !t->getSelfShadows()) return false;
    if ((forbidden & 0x40u) &&  t->getSelfShadows()) return false;

    const int type = t->getLightTypeEnum();
    if ((required & 0x3C00u) != 0
        && ((required & 0x400u)  == 0 || type != 0)
        && ((required & 0x800u)  == 0 || type != 1)
        && ((required & 0x1000u) == 0 || type != 2)
        && ((required & 0x2000u) == 0 || type != 3))
        return false;
    if ((forbidden & 0x3C00u) != 0
        && ((forbidden & 0x400u)  == 0 || type != 0)
        && ((forbidden & 0x800u)  == 0 || type != 1)
        && ((forbidden & 0x1000u) == 0 || type != 2)
        && ((forbidden & 0x2000u) == 0 || type != 3))
        return false;

    // The template header declares these two getters non-const (they are read-only bodies; the
    // ELF mangles them without the const qualifier), so the const pointer is cast for them alone.
    world::LightSourceTemplate* const mutableTemplate =
        const_cast<world::LightSourceTemplate*>(t);
    if ((required  & 0x4000u) && !mutableTemplate->getSupportsGlow()) return false;
    if ((forbidden & 0x4000u) &&  mutableTemplate->getSupportsGlow()) return false;
    if ((required  & 0x8000u) && !mutableTemplate->getSupportsEmitter()) return false;
    if ((forbidden & 0x8000u) &&  mutableTemplate->getSupportsEmitter()) return false;
    if ((required  & 0x10000u) && !t->getAffectLightmappedObjects()) return false;
    if ((forbidden & 0x10000u) &&  t->getAffectLightmappedObjects()) return false;
    return true;
}


// +0x14, sub_100F2E60 - every owned light that passes the masks, appended to `out` in set order.
// It walks the owned set at +0x1B8 (the head read is `[ecx+1BCh]`), not the active one:
// enabled-ness is a mask bit, not a set membership.

void RD9_THUNK lm_getLightsByMask(void* self RD9_THUNK_EDX, std::vector<world::LightSource*>* out,
                                   unsigned int required, unsigned int forbidden)
{
    const std::set<world::LightSource*>& owned = lm(self).ownedLights;
    for (std::set<world::LightSource*>::const_iterator node = owned.begin(); node != owned.end();
         ++node)
    {
        world::LightSource* const light = *node;
        if (lightMatchesMask(light, required, forbidden))
            out->push_back(light);
    }
}


// Appends this frame's visible lights matching required/forbidden masks.
// Required 0x7FFFFFFF instead selects enabled non-directional emitter lights and ignores forbidden.

// PE 0x100F2F40.
void RD9_THUNK lm_getVisibleLightsByMask(void* self RD9_THUNK_EDX, std::vector<world::LightSource*>* out,
                                          unsigned int required, unsigned int forbidden)
{
    std::vector<world::LightSource*>& scratch = lm(self).visibleLights;
    std::vector<world::LightSource*>& lights = *out;
    if (required == 0x7FFFFFFFu)
    {
        for (world::LightSource** it = scratch.data(); it != scratch.data() + scratch.size(); ++it)
        {
            world::LightSource* const light = *it;
            const world::LightSourceTemplate* const t = lightSourceTemplatePointer(light);
            // getSupportsEmitter is declared non-const on the template (lightMatchesMask's note).
            if (t->getLightTypeEnum() != 0 && light->getEnabled()
                && const_cast<world::LightSourceTemplate*>(t)->getSupportsEmitter())
                lights.push_back(light);
        }
        return;
    }
    for (world::LightSource** it = scratch.data(); it != scratch.data() + scratch.size(); ++it)
    {
        world::LightSource* const light = *it;
        if (lightMatchesMask(light, required, forbidden))
            lights.push_back(light);
    }
}


// First gather snapshots enabled static point lights into engine-heap clones and registers
// originals with the coordinator. A repeated initialization restores settings from those clones.

LightManager*        g_snapshotManager = 0;                       // dword_10264114
std::vector<world::LightSource*> g_snapshotOriginals;              // unk_10264118
std::vector<world::LightSource*> g_snapshotClones;                 // unk_10264128
bool                 g_snapshotRegistering = false;               // byte_10259BE4
bool                 g_gatherFirstCall = true;                    // byte_102395B4, .data 1

// PE 0x10194A10 (`retn 4`; the shipped body also leaves the unused this pointer in eax): binds the
// light manager the snapshot helper uses. [INFERRED] name.
void setSnapshotManager(LightManager* manager)
{
    g_snapshotManager = manager;
}

// sub_101949A0 - clone a light: its template's createObject, then copySettings from the original.
world::LightSource* cloneLightSettings(world::LightSource* light)
{
    world::LightSource* const clone = static_cast<world::LightSource*>(
        lightSourceTemplatePointer(light)->createObject());        // ObjectTemplate +0x50
    clone->copySettings(*light);                                     // LightSource +0x1E8
    return clone;
}


// PE 0x101954B0.
void collectSnapshotLights(std::vector<world::LightSource*>& lights)
{
    if (g_snapshotOriginals.size() != 0)
    {
        // The restore branch.
        const unsigned int count = static_cast<unsigned int>(g_snapshotOriginals.size());
        lights.reserve(count);
        world::LightSource** const originals = g_snapshotOriginals.data();
        for (unsigned int i = 0; i < count; ++i)
            lights.push_back(originals[i]);

        world::LightSource** const clones = g_snapshotClones.data();
        const unsigned int cloneCount = static_cast<unsigned int>(g_snapshotClones.size());
        world::LightSource** const first = lights.data();
        world::LightSource** const last  = lights.data() + lights.size();
        unsigned int c = 0;
        for (world::LightSource** it = first; it != last && c != cloneCount; ++it, ++c)
            (*it)->copySettings(*clones[c]);
        return;
    }

    const unsigned int before = static_cast<unsigned int>(lights.size());
    g_snapshotManager->getLightsByMask(lights, 0x805u, 0u);                    // +0x14

    world::LightSource** const first = lights.data() + before;
    world::LightSource** const last = lights.data() + lights.size();
    const unsigned int added = static_cast<unsigned int>(last - first);

    g_snapshotOriginals.reserve(added);
    g_snapshotClones.reserve(added);
    for (world::LightSource** it = first; it != last; ++it)
        g_snapshotOriginals.push_back(*it);
    for (world::LightSource** it = first; it != last; ++it)
    {
        world::LightSource* const clone = cloneLightSettings(*it);
        g_snapshotClones.push_back(clone);
    }
}


// PE 0x10195A30.
void snapshotAndRegisterStaticPointLights()
{
    std::vector<world::LightSource*> lights;
    collectSnapshotLights(lights);

    g_snapshotRegistering = true;
    for (world::LightSource** it = lights.data(); it != lights.data() + lights.size(); ++it)
        lmRegisterXpLight(*it);
    g_snapshotRegistering = false;
}

// The ray-test predicator the gather builds on its stack: sub_101C7E90 with mask 0x102, i.e.
// OFIsVisible | OFHasCollision, and 0x101C7EB0's rule that every bit of the mask must be present.
// Same two-flavour shape as ShadowCasterPredicator above, for the same reason.
class LightOcclusionPredicator : public world::ObjectPredicator
{
public:
    LightOcclusionPredicator() : m_mask(0x102u) {}
    virtual bool predicate(world::IObject* object)
    {
        return (m_mask & object->getFlags()) == m_mask;
    }
    virtual unsigned int getFlags() const { return m_mask; }
    virtual bool performCulling(world::IObject*) { return true; }
    virtual bool predicateSurface(int) { return true; }

private:
    unsigned int m_mask;
};

inline world::ObjectPredicator* lightOcclusionPredicatorPointer(LightOcclusionPredicator& p)
{
    return &p;
}


// Appends nearest eligible lights until out reaches maxLights.
// Candidates must strictly overlap the query sphere and pass both masks. Optional
// occlusion casts from the light toward position; hits inside radius remain accepted.

void RD9_THUNK lm_gatherLights(void* self RD9_THUNK_EDX, const Vec3* position, float radius,
                                std::vector<world::LightSource*>* out, unsigned int maxLights,
                                unsigned int required, unsigned int forbidden,
                                unsigned char occlusionTest)
{
    if (g_gatherFirstCall)
    {
        // Bind the light manager used by the snapshot helper (setSnapshotManager, PE 0x10194A10).
        setSnapshotManager(static_cast<LightManager*>(self));
        snapshotAndRegisterStaticPointLights();
        g_gatherFirstCall = false;
    }

    (void)dx9rend::rendererGetRenderView()->getViewMatrixPtr();

    std::vector<std::pair<float, world::LightSource*> > candidates;
    std::vector<world::LightSource*>& scratch = lm(self).visibleLights;
    world::LightSource** const first = scratch.data();
    world::LightSource** const last  = scratch.data() + scratch.size();
    for (world::LightSource** it = first; it != last; ++it)
    {
        world::LightSource* const light = *it;
        const float reach = light->getVisualBoundingRadius() + radius;
        const Vec3& p = light->getAbsolutePosition();
        const float dx = position->x - p.x;
        const float dy = position->y - p.y;
        const float dz = position->z - p.z;
        const float distanceSq = dz * dz + dy * dy + dx * dx;
        if (distanceSq < reach * reach && lightMatchesMask(light, required, forbidden))
            candidates.push_back(std::make_pair(distanceSq, light));
    }

    std::sort(candidates.begin(), candidates.end());

    const float radiusSq = radius * radius;
    world::ObjectManager* const manager = reinterpret_cast<world::ObjectManager*>(objectManager);
    for (std::vector<std::pair<float, world::LightSource*> >::iterator c = candidates.begin();
         c != candidates.end(); ++c)
    {
        if (out->size() >= maxLights)
            break;

        if (occlusionTest)
        {
            const Vec3& p = c->second->getAbsolutePosition();
            const Vec3 delta = { position->x - p.x, position->y - p.y, position->z - p.z };
            LightOcclusionPredicator predicate;                                 // sub_101C7E90(0x102)
            world::IObject* hitObject = 0;
            Vec3 hitPoint  = { 0.0f, 0.0f, 0.0f };
            Vec3 hitNormal = { 0.0f, 0.0f, 0.0f };
            int  hitPart;
            if (manager->intersectLine(hitObject, hitPoint, hitNormal, hitPart, p, delta,
                                       lightOcclusionPredicatorPointer(predicate),
                                       true, true, true, false, 0u))            // ObjectManager +0x68
            {
                const float hx = hitPoint.x - position->x;
                const float hy = hitPoint.y - position->y;
                const float hz = hitPoint.z - position->z;
                const float hitSq = hz * hz + hy * hy + hx * hx;
                if (hitSq > radiusSq)
                    continue;
            }
        }

        out->push_back(c->second);
    }
}


// +0x20, sub_100F1750 - gatherLights at a geometry instance.  ret 0x18.  The instance's transform
// (IGeometry +0x48, the +0x50 setter's stored pointer) supplies the position from its translation
// row; its bounding radius (IGeometry +0x44) the radius.  Both are reached by raw slot because the
// geometry instance interface is not modelled as a type in this tree (see ObjectDrawer.cpp); the
// call order - transform first, radius second - is the shipped one.

void RD9_THUNK lm_gatherLightsForGeometry(void* self RD9_THUNK_EDX, void* geometry,
                                           std::vector<world::LightSource*>* out,
                                           unsigned int maxLights,
                                           unsigned int required, unsigned int forbidden,
                                           int occlusionTest)
{
    typedef const Mat4* (__thiscall* TransformationFn)(void*);
    typedef float (__thiscall* BoundingRadiusFn)(void*);

    void** const vtbl = *reinterpret_cast<void***>(geometry);
    const Mat4* const transform =
        reinterpret_cast<TransformationFn>(vtbl[0x48 / 4])(geometry);
    const float radius =
        reinterpret_cast<BoundingRadiusFn>(vtbl[0x44 / 4])(geometry);
    const Vec3* const position = reinterpret_cast<const Vec3*>(&transform->m[12]);

    static_cast<LightManager*>(self)->gatherLights(*position, radius, *out, maxLights,
                                                   required, forbidden, occlusionTest != 0);
}

// +0x280, PE 0x1015BF40 and +0x2D8, the two one-instruction getters the StaticMesh
// shadow draw dispatches.  ; the fields are the ones the constructor seeds.
int RD9_THUNK lm_getStaticMeshShadowLod(void* self RD9_THUNK_EDX)
{
    return static_cast<int>(lm(self).staticMeshShadowLod);
}

int RD9_THUNK lm_getShadowMergeLodLimit(void* self RD9_THUNK_EDX)
{
    return static_cast<int>(lm(self).shadowMergeLodLimit);
}

// +0x10, PE 0x10160D30 -> 0x100F2D00. Retail clears the scratch vector, refreshes/copies the active
// RenderView's Frustum, applies the configured cull distance, and walks the active set in pointer
// order. Each accepted light is appended to the VC7.1 scratch vector at +0x1D0.
void RD9_THUNK lm_update(void* self RD9_THUNK_EDX, double time)
{
    (void)time;

    std::vector<world::LightSource*>& scratch = lm(self).visibleLights;
    // std::vector<LightSource*>::clear retains capacity; only the destructor frees the buffer.
    scratch.clear();

    LightManager* const lights = static_cast<LightManager*>(self);
    if (lights->getLightCullDistance() == 0.0f)
        return;

    dx9rend::IRenderView* const view = dx9rend::rendererGetRenderView();
    Frustum localFrustum(*view->getFrustumPtr());
    rendmath::frustumSetFarDist(localFrustum, lights->getLightCullDistance());

    const std::set<world::LightSource*>& active = lm(self).activeLights;
    std::vector<world::LightSource*>& visible = scratch;
    for (std::set<world::LightSource*>::const_iterator node = active.begin(); node != active.end();
         ++node)
    {
        world::LightSource* const light = *node;
        const float radius = light->getVisualBoundingRadius();
        const Vec3& position = light->getAbsolutePosition();
        if (rendmath::frustumInside(localFrustum, position, radius))
            visible.push_back(light);
    }

    static bool noted = false;
    if (!noted)
    {
        noted = true;
        ::renddx9abi::logf("LightManager::update: active-light cull path recovered; scratch reset, "
                           "RenderView frustum refresh and ordered sphere tests completed");
    }
}

// +0x24/+0x28, PE 0x1015BB90/0x1015BBC0.  Assignment owns one reference.  The same-pointer case
// deliberately adds another reference without releasing the old one; that retail leak is visible.
void RD9_THUNK lm_setSkyLight(void* self RD9_THUNK_EDX, world::LightSource* light)
{
    world::LightSource*& current = lm(self).skyLight;
    if (current && current != light)
        current->release();
    current = light;
    light->addRef();
}

world::LightSource* RD9_THUNK lm_getSkyLight(void* self RD9_THUNK_EDX)
{
    return lm(self).skyLight;
}

// +0x8C, PE 0x100F2C70.  addRef is unconditional after std::set::insert, including when a
// duplicate key is rejected (the shipped duplicate-registration leak).
void RD9_THUNK lm_addLight(void* self RD9_THUNK_EDX, world::LightSource* light)
{
    lm(self).ownedLights.insert(light);
    light->addRef();
}

// +0x90, PE 0x100F2B40.  The special pointer is non-owning. Sky ownership and owning-set
// ownership are independent, so a sky light found in the set is released twice in this call.
void RD9_THUNK lm_removeLight(void* self RD9_THUNK_EDX, world::LightSource* light)
{
    world::LightSource*& special = lm(self).sunLight;
    if (special == light)
        special = 0;

    LightManager* const manager = static_cast<LightManager*>(self);
    if (manager->getSkyLight() == light)
    {
        light->release();
        lm(self).skyLight = 0;
    }

    std::set<world::LightSource*>& owned = lm(self).ownedLights;
    const std::set<world::LightSource*>::iterator node = owned.find(light);
    if (node != owned.end())
    {
        (*node)->release();
        owned.erase(node);
    }
}

// +0x94, PE 0x100F2CA0.  Point/Spot lights visit the XpLight coordinator before every light is
// inserted into the non-owning active set.  The pointer at +0x840 is explicitly excluded.
void RD9_THUNK lm_enableLight(void* self RD9_THUNK_EDX, world::LightSource* light)
{
    if (lm(self).sunLight == light)
        return;

    const world::LightSourceTemplate* const objectTemplate =
        lightSourceTemplatePointer(light);
    const int lightType = objectTemplate->getLightTypeEnum();
    if (lightType == 1 || lightType == 2)
        lmRegisterXpLight(light);

    lm(self).activeLights.insert(light);
}

// +0x98, PE 0x100F2BB0.  Coordinator removal precedes erasing the active-set node and happens only
// when that node was present.
void RD9_THUNK lm_disableLight(void* self RD9_THUNK_EDX, world::LightSource* light)
{
    if (lm(self).sunLight == light)
        return;

    std::set<world::LightSource*>& active = lm(self).activeLights;
    const std::set<world::LightSource*>::iterator node = active.find(light);
    if (node != active.end())
    {
        lmRemoveXpLight(light);
        active.erase(node);
    }
}

// +0x54..+0x70 are exact setter/getter pairs over +0x13C..+0x148.
// PE 0x1015BD90.
void RD9_THUNK lm_setLightCullDistance(void* self RD9_THUNK_EDX, float value)
{
    lm(self).lightCullDistance = value;
}
float RD9_THUNK lm_getLightCullDistance(void* self RD9_THUNK_EDX)
{
    const float value = lm(self).lightCullDistance;
    return value > 0.0f ? value : 0.0f;
}
// PE 0x1015BDA0.
void RD9_THUNK lm_setMaxLightsPerBundledMesh(void* self RD9_THUNK_EDX, int value)
{
    lm(self).maxLightsPerBundledMesh = value;
}
int RD9_THUNK lm_getMaxLightsPerBundledMesh(void* self RD9_THUNK_EDX)
{
    const int value = lm(self).maxLightsPerBundledMesh;
    return value > 0 ? value : 0;
}
// PE 0x1015BDB0.
void RD9_THUNK lm_setMaxLightsPerStaticMesh(void* self RD9_THUNK_EDX, int value)
{
    lm(self).maxLightsPerStaticMesh = value;
}
int RD9_THUNK lm_getMaxLightsPerStaticMesh(void* self RD9_THUNK_EDX)
{
    const int value = lm(self).maxLightsPerStaticMesh;
    return value > 0 ? value : 0;
}
// PE 0x1015BDC0.
void RD9_THUNK lm_setMaxLightsPerSkinnedMesh(void* self RD9_THUNK_EDX, int value)
{
    lm(self).maxLightsPerSkinnedMesh = value;
}
int RD9_THUNK lm_getMaxLightsPerSkinnedMesh(void* self RD9_THUNK_EDX)
{
    const int value = lm(self).maxLightsPerSkinnedMesh;
    return value > 0 ? value : 0;
}
// PE 0x1015BDD0.
void RD9_THUNK lm_setMaxLightsPerTerrainPatch(void* self RD9_THUNK_EDX, int value)
{
    lm(self).maxLightsPerTerrainPatch = value;
}
int RD9_THUNK lm_getMaxLightsPerTerrainPatch(void* self RD9_THUNK_EDX)
{
    const int value = lm(self).maxLightsPerTerrainPatch;
    return value > 0 ? value : 0;
}

// Primary +0x7C/+0x80, PE 0x1015BF00/0x1015BF10. Unlike the neighbouring quality
// accessors, these preserve the stored float without clamping. The getter returns it in ST(0).
void RD9_THUNK lm_setSkinnedMeshShaderLodDistance(void* self RD9_THUNK_EDX, float value)
{
    lm(self).skinnedMeshShaderLodDistance = value;
}
float RD9_THUNK lm_getSkinnedMeshShaderLodDistance(void* self RD9_THUNK_EDX)
{
    return lm(self).skinnedMeshShaderLodDistance;
}
// Primary +0x84/+0x88: the getter returns the field's address; the setter reads and stores the
// second component before the first.
const Vec2& RD9_THUNK lm_getLightMapAmbientAlphaCutoffRange(void* self RD9_THUNK_EDX)
{
    return lm(self).lightMapAmbientAlphaCutoffRange;
}
// PE 0x1015CB60.
void RD9_THUNK lm_setLightMapAmbientAlphaCutoffRange(void* self RD9_THUNK_EDX, const Vec2& range)
{
    const float second = range.y;
    const float first = range.x;
    lm(self).lightMapAmbientAlphaCutoffRange.y = second;
    lm(self).lightMapAmbientAlphaCutoffRange.x = first;
}

// Simple-shadow settings and CPU map sampling. The packed map and bit-vector are populated by
// loadSimpleShadowMaps; retail has no null/bounds fallback in these texel readers.
// PE 0x1015BE10/20, table 0x1021AAF0 +0x284/+0x288; ordinary unclamped integer field.
void RD9_THUNK lm_setBundledMeshShadowLod(void* self RD9_THUNK_EDX, int lod)
{
    lm(self).bundledMeshShadowLod = lod;
}
int RD9_THUNK lm_getBundledMeshShadowLod(void* self RD9_THUNK_EDX)
{
    return lm(self).bundledMeshShadowLod;
}
// PE 0x1015BE70/80, +0x29C/+0x2A0. The upward ray length, default150 in the constructor.
void RD9_THUNK lm_setShadowRoofRayLength(void* self RD9_THUNK_EDX, float length)
{
    lm(self).shadowRoofRayLength = length;
}
float RD9_THUNK lm_getShadowRoofRayLength(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowRoofRayLength;
}
// PE 0x1015BE30/40, +0x28C/+0x290: the skinned-mesh shadow LOD is an unclamped integer.
void RD9_THUNK lm_setSkinnedMeshShadowLod(void* self RD9_THUNK_EDX, int lod)
{
    lm(self).skinnedMeshShadowLod = lod;
}
int RD9_THUNK lm_getSkinnedMeshShadowLod(void* self RD9_THUNK_EDX)
{
    return lm(self).skinnedMeshShadowLod;
}
// PE 0x1015BE90/A0, +0x2A4/+0x2A8: unclamped exponent.
void RD9_THUNK lm_setShadowSunScalePower(void* self RD9_THUNK_EDX, float power)
{
    lm(self).shadowSunScalePower = power;
}
float RD9_THUNK lm_getShadowSunScalePower(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowSunScalePower;
}
// PE 0x1015CB40 writes the second component first; 0x1015BEE0 returns the field's address.
void RD9_THUNK lm_setShadowSunHeightOverFade(void* self RD9_THUNK_EDX, const Vec2& range)
{
    const float first = range.x;
    lm(self).shadowSunHeightOverFade.y = range.y;
    lm(self).shadowSunHeightOverFade.x = first;
}
const Vec2& RD9_THUNK lm_getShadowSunHeightOverFade(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowSunHeightOverFade;
}

// PE 0x100F1F10, +0x2D0. Four two-bit cells per byte, low bits first. The arithmetic
// right shift of negative x is significant: the map origin is added before that byte offset.
float RD9_THUNK lm_sampleSimpleShadowMapTexel(void* self RD9_THUNK_EDX, int x, int z)
{
    const unsigned int row = (static_cast<unsigned int>(z) + lm(self).simpleShadowRowOrigin)
                          * lm(self).simpleShadowStride
                          + lm(self).simpleShadowColumnOrigin;
    const int byteIndex = static_cast<int>(row + static_cast<unsigned int>(x >> 2));
    const unsigned char packed = lm(self).simpleShadowMap[byteIndex];
    return static_cast<float>((packed >> (2 * (x & 3))) & 3) * 0.3333333432674408f;
}

// PE 0x100F2C10, +0x2CC. The IO map is a bit vector: compute the wrapped 32-bit bit index,
// then use a logical shift for its word address. The only results are the shipped 0 and 1.
float RD9_THUNK lm_sampleSimpleShadowMapIOTexel(void* self RD9_THUNK_EDX, int x, int z)
{
    const unsigned int bit =
        (static_cast<unsigned int>(z) + lm(self).simpleShadowIORowOrigin)
        * lm(self).simpleShadowIOStride + lm(self).simpleShadowIOColumnOrigin
        + static_cast<unsigned int>(x);
    const unsigned int word = lm(self).simpleShadowIOWords.data()[bit >> 5];
    return (word & (1u << (bit & 31u))) != 0 ? 1.0f : 0.0f;
}

// PE 0x100F1C70 (also 0x100F19D0): the two copies differ only in the virtual texel slot (+0x2D0 / +0x2CC).
// Keep their cell-centre offset, floor (not truncation), four-query order and float spills.
template <bool IoMap>
float sampleSimpleShadowMapFiltered(void* self, float x, float z)
{
    const float limit = lm(self).simpleShadowLimit;
    if (!(x >= -limit))
        x = -limit;
    else if (x > limit)
        x = limit;
    if (!(z >= -limit))
        z = -limit;
    else if (z > limit)
        z = limit;
    x = x * 0.5f - 0.5f;
    z = z * 0.5f - 0.5f;

    LightManager* const manager = static_cast<LightManager*>(self);
    const auto query = [manager](int cellX, int cellZ) -> float {
        return IoMap ? manager->sampleSimpleShadowMapIOTexel(cellX, cellZ)
                     : manager->sampleSimpleShadowMapTexel(cellX, cellZ);
    };
    const float lowerX = static_cast<float>(std::floor(static_cast<double>(x)));
    const float a = query(static_cast<int>(lowerX),
                         static_cast<int>(std::floor(static_cast<double>(z))));
    const float nextX = x + 1.0f;
    const float upperX = static_cast<float>(std::floor(static_cast<double>(x) + 1.0));
    const float b = query(static_cast<int>(upperX),
                         static_cast<int>(std::floor(static_cast<double>(z))));
    const float lowerXAgain = static_cast<float>(std::floor(static_cast<double>(x)));
    const float nextZ = z + 1.0f;
    const float c = query(static_cast<int>(lowerXAgain),
                         static_cast<int>(std::floor(static_cast<double>(nextZ))));
    const float upperXAgain = static_cast<float>(std::floor(static_cast<double>(nextX)));
    const float d = query(static_cast<int>(upperXAgain),
                         static_cast<int>(std::floor(static_cast<double>(nextZ))));
    x = static_cast<float>(static_cast<double>(x) - std::floor(static_cast<double>(x)));
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Retail keeps the bilinear expression in x87 until the caller stores it.
    typedef bf2x87::Real Real;
    const Real fractionZ = Real(z) - Real(std::floor(static_cast<double>(z)));
    const Real inverseX = Real(1.0f) - Real(x);
    return bf2x87::store((Real(a) * inverseX + Real(x) * Real(b)) * (Real(1.0f) - fractionZ)
        + (inverseX * Real(c) + Real(x) * Real(d)) * fractionZ);
#else
    const double fractionZ = static_cast<double>(z) - std::floor(static_cast<double>(z));
    const double inverseX = 1.0 - x;
    return static_cast<float>((a * inverseX + static_cast<double>(x) * b) * (1.0 - fractionZ)
                             + (inverseX * c + static_cast<double>(x) * d) * fractionZ);
#endif
}

// +0xEC, PE 0x1015CEC0. [RENDER] Retail created the point/spot shadow maps on enabling (the byte
// became the creation result) and cleared their records and released them on disabling; with those
// maps deleted the byte is only stored.
void RD9_THUNK lm_setDynamicShadowsEnabled(void* self RD9_THUNK_EDX, bool enabled)
{
    lm(self).dynamicShadowsEnabled = enabled ? 1 : 0;
}
bool RD9_THUNK lm_getDynamicShadowsEnabled(void* self RD9_THUNK_EDX)
{
    return lm(self).dynamicShadowsEnabled != 0;
}

// PE 0x1015C190.
void RD9_THUNK lm_setShadowIntensityBias(void* self RD9_THUNK_EDX, float value)
{
    lm(self).shadowIntensityBias = value;
    lm(self).shaderParameters[kParamShadowIntensityBias]->setFloat(value);
}
float RD9_THUNK lm_getShadowIntensityBias(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowIntensityBias;
}
// PE 0x1015C1C0.
void RD9_THUNK lm_setLightMapIntensityBias(void* self RD9_THUNK_EDX, float value)
{
    lm(self).lightMapIntensityBias = value;
    lm(self).shaderParameters[kParamLightMapIntensityBias]->setFloat(value);
}
float RD9_THUNK lm_getLightMapIntensityBias(void* self RD9_THUNK_EDX)
{
    return lm(self).lightMapIntensityBias;
}

// PE 0x1015C350.
void RD9_THUNK lm_setShadowUpdatesEnabled(void* self RD9_THUNK_EDX, bool enabled)
{
    lm(self).shadowUpdatesEnabled = enabled ? 1 : 0;
}
bool RD9_THUNK lm_getShadowUpdatesEnabled(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowUpdatesEnabled != 0;
}
// PE 0x1015BEC0.
void RD9_THUNK lm_setShadowUseSweepTest(void* self RD9_THUNK_EDX, bool enabled)
{
    lm(self).shadowUseSweepTest = enabled ? 1 : 0;
}
bool RD9_THUNK lm_getShadowUseSweepTest(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowUseSweepTest != 0;
}

// PE 0x1015C500, primary +0x188. The getter precedes the setter in the shipped table.
void RD9_THUNK lm_setShadowTSMFocus(void* self RD9_THUNK_EDX, float value)
{
    lm(self).shadowTSMFocus = value;
}
// PE 0x1015C4F0, primary +0x184.
float RD9_THUNK lm_getShadowTSMFocus(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowTSMFocus;
}

// +0xC4/+0xC8, PE 0x1015C150/0x1015C160.  The public spelling has not yet
// been attested, but the bodies and their use by the stock point-shadow
// predicate are exact: they write/read the feature byte at +0x268.
void RD9_THUNK lm_setShadowStaticFeature(void* self RD9_THUNK_EDX, bool enabled)
{
    lm(self).shadowStaticFeature = enabled ? 1 : 0;
}
bool RD9_THUNK lm_getShadowStaticFeature(void* self RD9_THUNK_EDX)
{
    return lm(self).shadowStaticFeature != 0;
}

// +0x19C, PE 0x101679A0. [RENDER] Retail rendered the directional (sun) shadow maps, then the
// point/spot maps, here. The sun cascades (RendDX9/SunShadows.cpp) replace the first; the second
// never drew outside the expansion pack (retail kept a light's records only under its policy
// byte, which PE 0x1018FB10 cleared there), so its pass, atlas and receivers are deleted
// (docs/lanes/render.md, milestone 6). The slot stays for the interface layout.
void RD9_THUNK lm_renderShadowMaps(void* RD9_THUNK_EDX)
{
}

// The stock Sky.con color surface.  Each setter is the same three-dword copy and each getter
// returns a reference to the in-object Vec3.  The distinct vtable slots and storage offsets are
// instruction-proven in the tiny PE bodies at 0x1015C5F0..0x1015C770 and
// Keeping the implementation templated does not merge ABI entries: MSVC
// still emits one __fastcall thunk per instantiated field offset.
// PE 0x1015C610 / 0x1015C630 / 0x1015C650 / 0x1015C670 / 0x1015C690 / 0x1015C6B0.
// PE 0x1015C6D0 / 0x1015C6F0 / 0x1015C720 / 0x1015C750 / 0x1015C770.
template <Vec3 LightManagerData::*Member>
void RD9_THUNK lm_setVec3(void* self RD9_THUNK_EDX, const Vec3* value)
{
    const float third = value->z;
    const float second = value->y;
    const float first = value->x;
    (lm(self).*Member).y = second;
    (lm(self).*Member).x = first;
    (lm(self).*Member).z = third;
}

template <Vec3 LightManagerData::*Member>
const Vec3* RD9_THUNK lm_getVec3(void* self RD9_THUNK_EDX)
{
    return &(lm(self).*Member);
}

inline bool vec3EpsilonEqual(const Vec3& a, const Vec3& b)
{
    // PE sub_10037DC0, called by setSunSpecColor before its coarser regeneration threshold.
    return std::fabs(a.x - b.x) <= FLT_EPSILON
        && std::fabs(a.y - b.y) <= FLT_EPSILON
        && std::fabs(a.z - b.z) <= FLT_EPSILON;
}

// PE 0x1015CB80/0x1015CC40, primary +0x1A0/+0x1A4. The manager interface takes and returns Vec3
// by value; the explicit output pointer below is the physical MSVC x86 sret parameter.
void RD9_THUNK lm_setSunDirection(void* self RD9_THUNK_EDX, Vec3 value)
{
    world::LightSource* const sun = lm(self).sunLight;
    sun->setDirection(value);
    dx9rend::textureReleaseRef(lm(self).normalCubeTexture);
    Vec3 zeroLight = { 0.0f, 0.0f, 0.0f };                    // [ebp-0xC], PE 0x1015CBC3
    dx9rend::EngineTexture* const normalCube = createNormalCubeTexture(256, zeroLight, false);
    lm(self).normalCubeTexture = normalCube;
    dx9rend::raShaderConstantsSetEngineTexture(0x2B8, normalCube);
}

// PE 0x1015CC40.
Vec3* RD9_THUNK lm_getSunDirection(void* self RD9_THUNK_EDX, Vec3* out)
{
    *out = lm(self).sunLight->getDirection();
    return out;
}

// +0x1A8 / +0x1AC
// PE 0x1015BF60.
void RD9_THUNK lm_setSunColor(void* self RD9_THUNK_EDX, Vec3 value)
{
    lm(self).sunLight->setColor(value);
}

// PE 0x1015CC70.
Vec3* RD9_THUNK lm_getSunColor(void* self RD9_THUNK_EDX, Vec3* out)
{
    *out = lm(self).sunLight->getColor();
    return out;
}

// PE 0x1015CCA0/0x1015CDD0, primary +0x1B0/+0x1B4. Exact epsilon equality skips every effect.
// Otherwise a squared delta >= 0.0025 (and unordered/NaN) regenerates one member of the four-LUT
// ring before the live LightSource receives the new specular colour.
void RD9_THUNK lm_setSunSpecColor(void* self RD9_THUNK_EDX, Vec3 value)
{
    world::LightSource* const sun = lm(self).sunLight;
    bool regenerate = lm(self).currentSpecularLut == 0;
    if (!regenerate)
    {
        if (vec3EpsilonEqual(sun->getSpecColor(), value))
            return;

        // Retail dispatches getSpecColor a second time for this coarser test.
        const Vec3& current = sun->getSpecColor();
        const float dx = current.x - value.x;
        const float dy = current.y - value.y;
        const float dz = current.z - value.z;
        regenerate = !(dx * dx + dy * dy + dz * dz < 0.0025f);
    }

    if (regenerate)
    {
        const unsigned int index = lm(self).specularLutIndex;
        dx9rend::EngineTexture* const lut = lm(self).specularLuts[index];
        lm(self).currentSpecularLut = lut;
        fillPowerLut(lut, 256, value, 64.0f, false);
        lm(self).specularLutIndex = (index + 1) & 3;
        dx9rend::raShaderConstantsSetEngineTexture(0x2C0, lut);
    }

    sun->setSpecColor(value);
}

// PE 0x1015CDD0.
Vec3* RD9_THUNK lm_getSunSpecColor(void* self RD9_THUNK_EDX, Vec3* out)
{
    *out = lm(self).sunLight->getSpecColor();
    return out;
}

// PE 0x1015BF80/0x1015BFB0, primary +0x1B8/+0x1BC. This is Object flag bit zero, not
// LightSource::setEnabled: true clears the disabled bit and false sets it.
void RD9_THUNK lm_setSunEnabled(void* self RD9_THUNK_EDX, bool enabled)
{
    world::LightSource* const sun = lm(self).sunLight;
    if (enabled)
        sun->updateFlags(0, 1);
    else
        sun->updateFlags(1, 0);
}

bool RD9_THUNK lm_getSunEnabled(void* self RD9_THUNK_EDX)
{
    return (lightSourceFlags(lm(self).sunLight) & 1u) == 0;
}

// Console property defaultEffectLightAffectionFactor: +0x26C setter, +0x270 getter.
// PE 0x1015BC70.
void RD9_THUNK lm_setDefaultEffectFactor(void* self RD9_THUNK_EDX, float value)
{
    lm(self).defaultEffectLightAffectionFactor = value;
}
float RD9_THUNK lm_getDefaultEffectFactor(void* self RD9_THUNK_EDX)
{
    return lm(self).defaultEffectLightAffectionFactor;
}

// Console property dynamicLightRangeScale: +0x2DC setter, +0x2E0 getter. PE 0x1015C5A0 and
// 0x1015C5B0 are direct stores/loads at +0x824; the constructor writes 1.0f.
void RD9_THUNK lm_setDynamicLightRangeScale(void* self RD9_THUNK_EDX, float value)
{
    lm(self).dynamicLightRangeScale = value;
}
float RD9_THUNK lm_getDynamicLightRangeScale(void* self RD9_THUNK_EDX)
{
    return lm(self).dynamicLightRangeScale;
}

// Console property occludedSkyNightIntensity: +0x2E4 setter, +0x2E8 getter. PE 0x1015C5C0 is
// exactly an x87 load from +0x828; zero-fill supplies the shipped constructor default 0.0f.
// PE 0x1015C5D0.
void RD9_THUNK lm_setOccludedSkyNightIntensity(void* self RD9_THUNK_EDX, float value)
{
    lm(self).occludedSkyNightIntensity = value;
}
float RD9_THUNK lm_getOccludedSkyNightIntensity(void* self RD9_THUNK_EDX)
{
    return lm(self).occludedSkyNightIntensity;
}

// PE 0x1015CE30, primary vtable +0x2F0. [RENDER] Retail kept the mesh renderers' shadow Effects
// here, per GeometryTemplate reflection type, for the point/spot shadow maps. Those maps and the
// registrations are deleted; the slot stays for the interface layout.
void RD9_THUNK lm_registerShadowEffect(void* RD9_THUNK_EDX, int, dx9rend::Effect*)
{
}

// PE 0x1015CE80 (also 0x1015CEA0, a second identical body): hands back an empty vector through the
// hidden result (first, last and end at +0x04/+0x08/+0x0C zeroed, the allocator byte untouched)
// and answers it; ECX and the second stack argument are never read (retn 8). [CHOSEN, not
// recovered] Unobservable-type exception: neither body has a code or data reference in the PE
// (no call, no vtable or table word) and the 2006 RendDX9_r match lists neither, so the result is
// always an empty vector that nothing receives; the name, the receiver, the argument's type and the
// element type cannot be observed (chosen: a free function over pointer elements, named for the
// LightManager methods the bodies sit between).
std::vector<const void*> lightManagerEmptyList(const void* receiver, int argument)
{
    (void)receiver;
    (void)argument;
    return std::vector<const void*>();
}

} // anonymous namespace



dx9rend::EngineTexture* lightManagerBlackCubeTexture(const LightManager* manager)
{
    return lm(manager).blackCubeTexture;
}

double lightManagerShadowFadeDuration(const LightManager* manager)
{
    return lm(manager).shadowFadeDuration;
}

// PE 0x1015CE00. Equality, smaller inputs and NaN all select the minimum duration.
void LightManager::setStaticLightBlendingTime(double duration)
{
    if (0.001 < duration)
        lm(this).shadowFadeDuration = duration;
    else
        lm(this).shadowFadeDuration = 0.001;
}

// PE 0x1015C5E0, shared with the mesh draws' duration read above.
double LightManager::getStaticLightBlendingTime() const
{
    return lightManagerShadowFadeDuration(this);
}

// PE 0x10005920. The base renderer's render-target selector: __thiscall on `base`, four stack
// arguments (retn 0x10), always returns true (al = 1). With a texture, render target `index`
// becomes the texture's (level, face) surface (sub_10014950, returned referenced). Without one,
// target 0 takes the base's temporary back buffer (+0xF4) or else the Renderer's back buffer
// (dword_1023D098 +0x76C), each AddRef'd; any other index is cleared. StateManager +0x60 binds the
// surface and the temporary reference is then dropped. Retail reads +0xF4 and +0x76C without a
// null test of the objects that hold them.
bool lightManagerSelectRenderTarget(dx9rend::RendDX9Base* base, unsigned int index,
                                    dx9rend::EngineTexture* texture, unsigned int level,
                                    unsigned int face)
{
    IDirect3DSurface9* surface = 0;
    if (texture)
        surface = dx9rend::textureGetLevelSurface(texture, level, face);
    else if (index == 0)
    {
        surface = base->tempBackBuffer;                                     // +0x0F4
        if (!surface)
            surface = dx9rend::rendererSceneColorSurface();                 // +0x76C; [RENDER] HDR
        if (surface)
            surface->AddRef();
    }
    dx9rend::stateManagerSetRenderTarget(index, surface);                   // StateManager +0x60
    if (surface)
        surface->Release();
    return true;
}

// The same retail function as lightManagerSelectRenderTarget above, not a separate one: the base
// renderer and the level 0 / face 0x7FFFFFFF arguments every geometry and post-production caller
// pushes. Published for those callers (XpLightManager.h).
void lightManagerSetRenderTargetTexture(unsigned int index, dx9rend::EngineTexture* texture)
{
    lightManagerSelectRenderTarget(static_cast<dx9rend::RendDX9Base*>(dx9rend::g_rendererBase),
                                   index, texture, 0, 0x7FFFFFFFu);
}

void lightManagerRestoreRendererTarget0()
{
    restoreRendererTarget0();
}

void lightManagerSetDepthStencil(IDirect3DSurface9* surface)
{
    setLightManagerDepthStencil(surface);
}

// Source-level LightManager implementation. The public type deliberately has ordinary virtual
// methods: BF2.exe callers (notably LightSource) dispatch through the recovered primary interface,
// while RendDX9 code can use the concrete type directly. The HostAbi adapters below exist only
// because 89 still-unknown signatures require the explicit 189-entry table to remain the DLL
// boundary; no source caller indexes that table.
void LightManager::addRef()
{
    (void)lm_addRef(this RD9_THUNK_ARG);
}

int LightManager::getRef()
{
    return lm_getRef(this RD9_THUNK_ARG);
}

int LightManager::release()
{
    return lm_release(this RD9_THUNK_ARG);
}

void* LightManager::queryInterface(unsigned int iid) const
{
    return lm_queryInterface(const_cast<LightManager*>(this) RD9_THUNK_ARG, iid);
}

void LightManager::update(double time)
{
    lm_update(this RD9_THUNK_ARG, time);
}

void LightManager::getLightsByMask(std::vector<world::LightSource*>& out, unsigned int required,
                                   unsigned int forbidden)
{
    lm_getLightsByMask(this RD9_THUNK_ARG, &out, required, forbidden);
}

void LightManager::getVisibleLightsByMask(std::vector<world::LightSource*>& out,
                                          unsigned int required, unsigned int forbidden)
{
    lm_getVisibleLightsByMask(this RD9_THUNK_ARG, &out, required, forbidden);
}

void LightManager::gatherLights(const Vec3& position, float radius,
                                std::vector<world::LightSource*>& out,
                                unsigned int maxLights, unsigned int required,
                                unsigned int forbidden, bool occlusionTest)
{
    lm_gatherLights(this RD9_THUNK_ARG, &position, radius, &out, maxLights, required, forbidden,
                    occlusionTest ? 1u : 0u);
}

void LightManager::gatherLightsForGeometry(void* geometry, std::vector<world::LightSource*>& out,
                                           unsigned int maxLights, unsigned int required,
                                           unsigned int forbidden, bool occlusionTest)
{
    lm_gatherLightsForGeometry(this RD9_THUNK_ARG, geometry, &out, maxLights, required, forbidden,
                               occlusionTest ? 1 : 0);
}

int LightManager::getStaticMeshShadowLod() const
{
    return lm_getStaticMeshShadowLod(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

int LightManager::getShadowMergeLodLimit() const
{
    return lm_getShadowMergeLodLimit(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setSkyLight(world::LightSource* light)
{
    lm_setSkyLight(this RD9_THUNK_ARG, light);
}

world::LightSource* LightManager::getSkyLight() const
{
    return lm_getSkyLight(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

#define BF2_LIGHT_DEFINE_COLOR_PROPERTY(Name, Member)                              \
    void LightManager::set##Name(const Vec3& color)                                \
    {                                                                               \
        lm_setVec3<&LightManagerData::Member>(this RD9_THUNK_ARG, &color);          \
    }                                                                               \
    const Vec3& LightManager::get##Name() const                                     \
    {                                                                               \
        return *lm_getVec3<&LightManagerData::Member>(const_cast<LightManager*>(this) RD9_THUNK_ARG); \
    }

BF2_LIGHT_DEFINE_COLOR_PROPERTY(SkyColor, skyColor)                       // +0x018
BF2_LIGHT_DEFINE_COLOR_PROPERTY(AmbientColor, ambientColor)               // +0x03C
BF2_LIGHT_DEFINE_COLOR_PROPERTY(TreeAmbientColor, treeAmbientColor)       // +0x048
BF2_LIGHT_DEFINE_COLOR_PROPERTY(TreeSunColor, treeSunColor)               // +0x024
BF2_LIGHT_DEFINE_COLOR_PROPERTY(TreeSkyColor, treeSkyColor)               // +0x030
BF2_LIGHT_DEFINE_COLOR_PROPERTY(StaticSunColor, staticSunColor)           // +0x054
BF2_LIGHT_DEFINE_COLOR_PROPERTY(StaticSpecularColor, staticSpecularColor) // +0x06C
BF2_LIGHT_DEFINE_COLOR_PROPERTY(StaticSkyColor, staticSkyColor)           // +0x060
BF2_LIGHT_DEFINE_COLOR_PROPERTY(SinglePointColor, singlePointColor)       // +0x078
BF2_LIGHT_DEFINE_COLOR_PROPERTY(DynamicPointColor, dynamicPointColor)     // +0x084
BF2_LIGHT_DEFINE_COLOR_PROPERTY(EffectSunColor, effectSunColor)           // +0x120
BF2_LIGHT_DEFINE_COLOR_PROPERTY(EffectShadowColor, effectShadowColor)     // +0x12C

#undef BF2_LIGHT_DEFINE_COLOR_PROPERTY

// +0x4C/+0x50, PE 0x1015BD70/0x1015BD80. Both retail bodies ignore LightManager `this` and
// forward through the typed HemiMapManager singleton without a null guard. These are real
// LightManager member bodies; neither side of the call indexes a vtable manually.
void LightManager::setHemiLerpBias(float bias)
{
    g_HemiMapManager->setHemiLerpBias(bias);
}

float LightManager::getHemiLerpBias() const
{
    return g_HemiMapManager->getHemiLerpBias();
}

namespace {

// SettingsRepostitory +0x10, `const int& intGet(const std::string&, const int&)`, exactly as PE
// 0x1015CA20 makes it: `mov ecx, dword_10244A90` with no null test, and the answer dereferenced
// (`fild dword ptr [eax]`). [PLATFORM] carrier split as simpleShadowStringSetting's above: the native
// flavour calls the recovered SettingsRepostitory declaration, the retail flavour its +0x10 slot with
// the VC7.1 HostString key.
const int& groundHemiIntSetting(const renddx9abi::HostString& key, const int& fallback)
{
    return settings->intGet(key, fallback);
}

} // anonymous namespace

// +0x44, PE 0x1015C9E0 (112 B, `retn 4`), `this` is never read - the
// body works on the published HemiMapManager (`mov ecx, g_HemiMapManager`, no null test), like the
// +0x4C/+0x50 bias pair above. In shipped order: the default 2048 stored, the key "GLSWorldSizeX"
// built, the origin zeroed, SettingsRepostitory +0x10 asked for the world size (GLSWorldSizeX - the
// heightmap size GameLogic::loadInitFile publishes, per the editor's lighting census §5.7), its int
// converted with `fild`, then setBaseHemiMap(name, 0/0/0, size, 500.0f) - the same falloff the
// setBaseHemiMap binding supplies for a missing fourth argument.
void LightManager::setGroundHemi(const LightManagerHostString& name)
{
    const int defaultWorldSize = 2048;
    const renddx9abi::HostString key("GLSWorldSizeX");
    Vec3 origin;
    origin.x = 0.0f;
    origin.y = 0.0f;
    origin.z = 0.0f;
    const float range = static_cast<float>(groundHemiIntSetting(key, defaultWorldSize));
    g_HemiMapManager->setBaseHemiMap(name, origin, range, 500.0f);
}

// +0x48, PE 0x1015CA50 (68 B), The guarded function-local static
// empty string (dword_102605D8 bit 0, atexit 0x101D9990) is constructed on the first call, before the
// HemiMapManager's +0x18 getBaseHemiMap is asked; a base map answers its name (+0x18), none the
// static. `this` is never read.
const std::string& LightManager::getGroundHemi() const
{
    static const std::string s_noGroundHemi;
    const HemiMap* const baseMap = g_HemiMapManager->getBaseHemiMap();
    if (baseMap)
        return baseMap->name;
    return s_noGroundHemi;
}

void LightManager::setLightCullDistance(float distance)
{
    lm_setLightCullDistance(this RD9_THUNK_ARG, distance);
}

float LightManager::getLightCullDistance() const
{
    return lm_getLightCullDistance(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setMaxLightsPerBundledMesh(int quality)
{
    lm_setMaxLightsPerBundledMesh(this RD9_THUNK_ARG, quality);
}

int LightManager::getMaxLightsPerBundledMesh() const
{
    return lm_getMaxLightsPerBundledMesh(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setMaxLightsPerStaticMesh(int quality)
{
    lm_setMaxLightsPerStaticMesh(this RD9_THUNK_ARG, quality);
}

int LightManager::getMaxLightsPerStaticMesh() const
{
    return lm_getMaxLightsPerStaticMesh(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setMaxLightsPerSkinnedMesh(int quality)
{
    lm_setMaxLightsPerSkinnedMesh(this RD9_THUNK_ARG, quality);
}

int LightManager::getMaxLightsPerSkinnedMesh() const
{
    return lm_getMaxLightsPerSkinnedMesh(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setMaxLightsPerTerrainPatch(int quality)
{
    lm_setMaxLightsPerTerrainPatch(this RD9_THUNK_ARG, quality);
}

int LightManager::getMaxLightsPerTerrainPatch() const
{
    return lm_getMaxLightsPerTerrainPatch(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setSkinnedMeshShaderLodDistance(float distance)
{
    lm_setSkinnedMeshShaderLodDistance(this RD9_THUNK_ARG, distance);
}

float LightManager::getSkinnedMeshShaderLodDistance() const
{
    return lm_getSkinnedMeshShaderLodDistance(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

const Vec2& LightManager::getLightMapAmbientAlphaCutoffRange() const
{
    return lm_getLightMapAmbientAlphaCutoffRange(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setLightMapAmbientAlphaCutoffRange(const Vec2& range)
{
    lm_setLightMapAmbientAlphaCutoffRange(this RD9_THUNK_ARG, range);
}

void LightManager::setBundledMeshShadowLod(int lod)
{
    lm_setBundledMeshShadowLod(this RD9_THUNK_ARG, lod);
}
int LightManager::getBundledMeshShadowLod() const
{
    return lm_getBundledMeshShadowLod(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}
void LightManager::setShadowRoofRayLength(float length)
{
    lm_setShadowRoofRayLength(this RD9_THUNK_ARG, length);
}
float LightManager::getShadowRoofRayLength() const
{
    return lm_getShadowRoofRayLength(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}
void LightManager::setSkinnedMeshShadowLod(int lod)
{
    lm_setSkinnedMeshShadowLod(this RD9_THUNK_ARG, lod);
}
int LightManager::getSkinnedMeshShadowLod() const
{
    return lm_getSkinnedMeshShadowLod(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}
void LightManager::setShadowSunScalePower(float power)
{
    lm_setShadowSunScalePower(this RD9_THUNK_ARG, power);
}
float LightManager::getShadowSunScalePower() const
{
    return lm_getShadowSunScalePower(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}
void LightManager::setShadowSunHeightOverFade(const Vec2& range)
{
    lm_setShadowSunHeightOverFade(this RD9_THUNK_ARG, range);
}
const Vec2& LightManager::getShadowSunHeightOverFade() const
{
    return lm_getShadowSunHeightOverFade(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}
float LightManager::sampleSimpleShadowMap(float x, float z) const
{
    return sampleSimpleShadowMapFiltered<false>(const_cast<LightManager*>(this), x, z);
}
float LightManager::sampleSimpleShadowMapIO(float x, float z) const
{
    return sampleSimpleShadowMapFiltered<true>(const_cast<LightManager*>(this), x, z);
}
float LightManager::sampleSimpleShadowMapIOTexel(int x, int z) const
{
    return lm_sampleSimpleShadowMapIOTexel(const_cast<LightManager*>(this) RD9_THUNK_ARG, x, z);
}
float LightManager::sampleSimpleShadowMapTexel(int x, int z) const
{
    return lm_sampleSimpleShadowMapTexel(const_cast<LightManager*>(this) RD9_THUNK_ARG, x, z);
}

void LightManager::addLight(world::LightSource* light)
{
    lm_addLight(this RD9_THUNK_ARG, light);
}

void LightManager::removeLight(world::LightSource* light)
{
    lm_removeLight(this RD9_THUNK_ARG, light);
}

void LightManager::enableLight(world::LightSource* light)
{
    lm_enableLight(this RD9_THUNK_ARG, light);
}

void LightManager::disableLight(world::LightSource* light)
{
    lm_disableLight(this RD9_THUNK_ARG, light);
}

// PE 0x1015C7A0.
void LightManager::setSinglePointColorHigh(const Vec3& color)
{
    lm(this).blendedColors[0] = color;
}

// PE 0x1015BCB0.
const Vec3& LightManager::getSinglePointColorHigh() const
{
    return lm(this).blendedColors[0];
}

// PE 0x1015C7D0.
void LightManager::setSinglePointColorLow(const Vec3& color)
{
    lm(this).blendedColors[1] = color;
}

// PE 0x1015BCC0.
const Vec3& LightManager::getSinglePointColorLow() const
{
    return lm(this).blendedColors[1];
}

// PE 0x1015C800.
void LightManager::setDynamicPointColorHigh(const Vec3& color)
{
    lm(this).blendedColors[2] = color;
}

// PE 0x1015BCD0.
const Vec3& LightManager::getDynamicPointColorHigh() const
{
    return lm(this).blendedColors[2];
}

// PE 0x1015C830.
void LightManager::setDynamicPointColorLow(const Vec3& color)
{
    lm(this).blendedColors[3] = color;
}

// PE 0x1015BCE0.
const Vec3& LightManager::getDynamicPointColorLow() const
{
    return lm(this).blendedColors[3];
}

// PE 0x1015C860.
void LightManager::setSkyColorHigh(const Vec3& color)
{
    lm(this).blendedColors[4] = color;
}

// PE 0x1015BCF0.
const Vec3& LightManager::getSkyColorHigh() const
{
    return lm(this).blendedColors[4];
}

// PE 0x1015C890.
void LightManager::setSkyColorLow(const Vec3& color)
{
    lm(this).blendedColors[5] = color;
}

// PE 0x1015BD00.
const Vec3& LightManager::getSkyColorLow() const
{
    return lm(this).blendedColors[5];
}

// PE 0x1015C8C0.
void LightManager::setStaticSkyColorHigh(const Vec3& color)
{
    lm(this).blendedColors[6] = color;
}

// PE 0x1015BD10.
const Vec3& LightManager::getStaticSkyColorHigh() const
{
    return lm(this).blendedColors[6];
}

// PE 0x1015C8F0.
void LightManager::setStaticSkyColorLow(const Vec3& color)
{
    lm(this).blendedColors[7] = color;
}

// PE 0x1015BD20.
const Vec3& LightManager::getStaticSkyColorLow() const
{
    return lm(this).blendedColors[7];
}

// PE 0x1015C920.
void LightManager::setSunColorHigh(const Vec3& color)
{
    lm(this).blendedColors[8] = color;
}

// PE 0x1015BD30.
const Vec3& LightManager::getSunColorHigh() const
{
    return lm(this).blendedColors[8];
}

// PE 0x1015C950.
void LightManager::setSunColorLow(const Vec3& color)
{
    lm(this).blendedColors[9] = color;
}

// PE 0x1015BD40.
const Vec3& LightManager::getSunColorLow() const
{
    return lm(this).blendedColors[9];
}

// PE 0x1015C980.
void LightManager::setSunSpecColorHigh(const Vec3& color)
{
    lm(this).blendedColors[10] = color;
}

// PE 0x1015BD50.
const Vec3& LightManager::getSunSpecColorHigh() const
{
    return lm(this).blendedColors[10];
}

// PE 0x1015C9B0.
void LightManager::setSunSpecColorLow(const Vec3& color)
{
    lm(this).blendedColors[11] = color;
}

// PE 0x1015BD60.
const Vec3& LightManager::getSunSpecColorLow() const
{
    return lm(this).blendedColors[11];
}

// PE 0x100F3050; 0x100F6A60 inserts without rejecting equal float keys.
LightDistanceMap* LightManager::getVisibleLightsByDistance(unsigned int required,
                                                         unsigned int forbidden)
{
    const Mat4* const camera = dx9rend::rendererGetRenderView()->getViewMatrixPtr();
    LightDistanceMap& output = lm(this).lightsByDistance;
    output.clear();
    std::vector<world::LightSource*>& lights = lm(this).distanceLights;
    lights.clear();
    getVisibleLightsByMask(lights, required, forbidden);
    for (std::vector<world::LightSource*>::iterator it = lights.begin(); it != lights.end(); ++it)
    {
        world::LightSource* const light = *it;
        const Vec3& position = light->getAbsolutePosition();
#if defined(__APPLE__) || defined(BF2_TEST_SOFT_EXTENDED)
        // [CHOSEN, not recovered] Preserve the PE's explicit x store and the retained y/z
        // intermediates through the native extended-arithmetic policy.
        const float x = bf2x87::store(bf2x87::sub(bf2x87::wide(camera->m[12]),
                                               bf2x87::wide(position.x)));
        const bf2x87::Real y = bf2x87::sub(bf2x87::wide(camera->m[13]), bf2x87::wide(position.y));
        const bf2x87::Real z = bf2x87::sub(bf2x87::wide(camera->m[14]), bf2x87::wide(position.z));
        const float distanceSquared = bf2x87::store(bf2x87::add(
            bf2x87::add(bf2x87::mul(z, z), bf2x87::mul(bf2x87::wide(x), bf2x87::wide(x))),
            bf2x87::mul(y, y)));
#else
        const float x = camera->m[12] - position.x;
        const float y = camera->m[13] - position.y;
        const float z = camera->m[14] - position.z;
        const float distanceSquared = (z * z + x * x) + y * y;
#endif
        output.insert(std::make_pair(distanceSquared, light));
    }
    return &output;
}

// PE 0x1015C280. [RENDER] Retail also set SHADOWALPHATHRESHOLD on the skinned and bundled shadow
// Effects, deleted with the point/spot shadow maps; the threshold is only stored.
void LightManager::setAlphaShadowThreshold(float threshold)
{
    lm(this).alphaShadowThreshold = threshold;
}

// PE 0x1015C300.
float LightManager::getAlphaShadowThreshold() const
{
    return lm(this).alphaShadowThreshold;
}

// PE 0x1015BFC0. [RENDER] Retail recreated the point/spot shadow maps and their atlas at the new
// size (restoring the old size on failure); with those maps deleted the size is only stored.
void LightManager::setShadowMapSize(int size)
{
    lm(this).shadowMapSize = size;
}

// PE 0x1015C000.
int LightManager::getShadowMapSize() const
{
    return lm(this).shadowMapSize;
}

// PE 0x1015C010. The comparison is signed and there is no lower bound. [RENDER] Retail then
// rebuilt the point/spot shadow atlas, which is deleted.
void LightManager::setShadowMapObjectSize(int size)
{
    if (size <= lm(this).shadowMapSize)
        lm(this).shadowTileSize = size;
}

// PE 0x1015C030.
int LightManager::getShadowMapObjectSize() const
{
    return lm(this).shadowTileSize;
}

// PE 0x1015C040. [RENDER] Retail recreated the point/spot shadow maps in the new format (restoring
// the old one on failure); with those maps deleted the format is only stored.
void LightManager::setShadowMapBitDepth(int depth)
{
    switch (depth)
    {
    case 16: lm(this).shadowMapFormat = 0x50; break;
    case 24: lm(this).shadowMapFormat = 0x4D; break;
    case 32: lm(this).shadowMapFormat = 0x72; break;
    }
}

// PE 0x1015C0A0.
int LightManager::getShadowMapBitDepth() const
{
    switch (lm(this).shadowMapFormat)
    {
    case 0x4B:
    case 0x4D: return 24;
    case 0x50: return 16;
    case 0x72: return 32;
    default: return 0;
    }
}

// PE 0x1015C1F0.
void LightManager::setDropShadowClipHeightOffset(float value)
{
    lm(this).dropShadowClipHeightOffset = value;
}

// PE 0x1015C200.
float LightManager::getDropShadowClipHeightOffset() const
{
    return lm(this).dropShadowClipHeightOffset;
}

// PE 0x1015C220.
void LightManager::setDynamicStaticMeshShadowsEnabled(bool value)
{
    lm(this).dynamicStaticMeshShadowsEnabled = value;
}

// PE 0x1015C230.
bool LightManager::getDynamicStaticMeshShadowsEnabled() const
{
    return lm(this).dynamicStaticMeshShadowsEnabled != 0;
}

// PE 0x1015C240.
void LightManager::setDynamic1pShadowsEnabled(bool value)
{
    lm(this).firstPersonShadows = value;
}

// PE 0x1015C250.
bool LightManager::getDynamic1pShadowsEnabled() const
{
    return lm(this).firstPersonShadows != 0;
}

// PE 0x1015C260.
void LightManager::setAllowAlphaShadows(bool value)
{
    lm(this).allowAlphaShadows = value;
}

// PE 0x1015C270.
bool LightManager::getAllowAlphaShadows() const
{
    return lm(this).allowAlphaShadows != 0;
}

// PE 0x1015C310.
void LightManager::setShadowUpdateFactor(float value)
{
    lm(this).shadowUpdateFactor = value;
}

// PE 0x1015C320.
float LightManager::getShadowUpdateFactor() const
{
    return lm(this).shadowUpdateFactor;
}

// PE 0x1015C330.
void LightManager::setStencilCullerResetEnabled(bool enabled)
{
    lm(this).stencilCullerResetEnabled = enabled;
}

// PE 0x1015C340.
bool LightManager::getStencilCullerResetEnabled() const
{
    return lm(this).stencilCullerResetEnabled != 0;
}

// PE 0x1015C3C0.
void LightManager::setShadowOccluderOversizeFactor(float value)
{
    lm(this).shadowOccluderOversizeFactor = value;
}

// PE 0x1015C3B0.
float LightManager::getShadowOccluderOversizeFactor() const
{
    return lm(this).shadowOccluderOversizeFactor;
}

// PE 0x1015C3E0.
void LightManager::setUseLSSMOptimizations(bool value)
{
    lm(this).shadowProjectionFlags[0] = value;
    if (value)
        setUsePSMOptimizations(false);
}

// PE 0x1015C3D0.
bool LightManager::getUseLSSMOptimizations() const
{
    return lm(this).shadowProjectionFlags[0] != 0;
}

// PE 0x1015C410.
void LightManager::setUsePSMOptimizations(bool value)
{
    lm(this).shadowProjectionFlags[1] = value;
    if (value)
        setUseLSSMOptimizations(false);
}

// PE 0x1015C400.
bool LightManager::getUsePSMOptimizations() const
{
    return lm(this).shadowProjectionFlags[1] != 0;
}

// PE 0x1015C4A0.
void LightManager::setShadowPSMUseSlideback(bool value)
{
    lm(this).shadowProjectionFlags[2] = value;
}

// PE 0x1015C490.
bool LightManager::getShadowPSMUseSlideback() const
{
    return lm(this).shadowProjectionFlags[2] != 0;
}

// PE 0x1015C4C0.
void LightManager::setShadowPSMUseUnitCubeClipping(bool value)
{
    lm(this).shadowProjectionFlags[3] = value;
}

// PE 0x1015C4B0.
bool LightManager::getShadowPSMUseUnitCubeClipping() const
{
    return lm(this).shadowProjectionFlags[3] != 0;
}

// PE 0x1015C440.
void LightManager::setShadowPSMZNearOffset(float value)
{
    lm(this).shadowPSMZNearOffset = value;
}

// PE 0x1015C430.
float LightManager::getShadowPSMZNearOffset() const
{
    return lm(this).shadowPSMZNearOffset;
}

// PE 0x1015C460.
void LightManager::setShadowPSMZFarOffset(float value)
{
    lm(this).shadowPSMZFarOffset = value;
}

// PE 0x1015C450.
float LightManager::getShadowPSMZFarOffset() const
{
    return lm(this).shadowPSMZFarOffset;
}

// PE 0x1015C480.
void LightManager::setShadowPSMZMinInfinityZ(float value)
{
    lm(this).shadowPSMZMinInfinityZ = value;
}

// PE 0x1015C470.
float LightManager::getShadowPSMZMinInfinityZ() const
{
    return lm(this).shadowPSMZMinInfinityZ;
}

// PE 0x1015C4E0.
void LightManager::setShadowPSMFovY(float value)
{
    lm(this).shadowPSMFovY = value;
}

// PE 0x1015C4D0.
float LightManager::getShadowPSMFovY() const
{
    return lm(this).shadowPSMFovY;
}

// PE 0x1015C520.
void LightManager::setShadow80Line(float value)
{
    lm(this).shadow80Line = value;
}

// PE 0x1015C510.
float LightManager::getShadow80Line() const
{
    return lm(this).shadow80Line;
}

// PE 0x1015C530.
void LightManager::setShadowMergeDynamicShadowFrustums(bool value)
{
    lm(this).shadowMergeDynamicShadowFrustums = value;
}

// PE 0x1015C540.
bool LightManager::getShadowMergeDynamicShadowFrustums() const
{
    return lm(this).shadowMergeDynamicShadowFrustums != 0;
}

// PE 0x1015BE50.
void LightManager::setShadowOverrideQuality(int value)
{
    lm(this).shadowOverrideQuality = value;
}

// PE 0x1015BE60.
int LightManager::getShadowOverrideQuality() const
{
    return lm(this).shadowOverrideQuality;
}

// PE 0x1015C110.
void LightManager::setShadowDistance(float distance)
{
    lm(this).shadowDistance = distance;
}

// PE 0x1015C120.
float LightManager::getShadowDistance() const
{
    return lm(this).shadowDistance;
}

// PE 0x1015C130. [CHOSEN, not recovered] name; see the declaration (unobservable-type exception).
void LightManager::setUnusedFloat218(float value)
{
    lm(this).unresolved218 = value;
}

// PE 0x1015C140. [CHOSEN, not recovered] name; see the declaration.
float LightManager::getUnusedFloat218() const
{
    return lm(this).unresolved218;
}

// PE 0x1015C170.
void LightManager::setShadowShowFrustums(bool enabled)
{
    lm(this).showFrustums = enabled;
}

// PE 0x1015C180.
bool LightManager::getShadowShowFrustums() const
{
    return lm(this).showFrustums != 0;
}

// PE 0x1015C370.
void LightManager::setShadowDepthBias(float bias)
{
    lm(this).shadowDepthBias = bias;
}

// PE 0x1015C380.
float LightManager::getShadowDepthBias() const
{
    return lm(this).shadowDepthBias;
}

// PE 0x1015C390.
void LightManager::setShadowSlopeScaleDepthBias(float bias)
{
    lm(this).shadowSlopeScaleDepthBias = bias;
}

// PE 0x1015C3A0.
float LightManager::getShadowSlopeScaleDepthBias() const
{
    return lm(this).shadowSlopeScaleDepthBias;
}

// PE 0x1015BDE0. Values outside the signed interval [0, 3) leave the previous LOD intact.
void LightManager::setTerrainShadowLod(int lod)
{
    if (lod >= 0 && lod < 3)
        lm(this).terrainShadowLod = static_cast<unsigned int>(lod);
}

// PE 0x1015BE00.
int LightManager::getTerrainShadowLod() const
{
    return static_cast<int>(lm(this).terrainShadowLod);
}

// PE 0x1015BF20. Negative values are refused; there is no upper limit.
void LightManager::setStaticMeshShadowLod(int lod)
{
    if (lod >= 0)
        lm(this).staticMeshShadowLod = static_cast<unsigned int>(lod);
}

// PE 0x1015BEB0.
void LightManager::setShadowPullbackDistance(float distance)
{
    lm(this).shadowPullbackDistance = distance;
}

// PE 0x1015BEF0.
float LightManager::getShadowPullbackDistance() const
{
    return lm(this).shadowPullbackDistance;
}

// PE 0x1015C580. The shipped setter stores the complete word without clamping.
void LightManager::setShadowMergeLodLimit(int lod)
{
    lm(this).shadowMergeLodLimit = static_cast<unsigned int>(lod);
}

void LightManager::setShadowStaticFeature(bool enabled)
{
    lm_setShadowStaticFeature(this RD9_THUNK_ARG, enabled);
}

bool LightManager::getShadowStaticFeature() const
{
    return lm_getShadowStaticFeature(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setShadowIntensityBias(float value)
{
    lm_setShadowIntensityBias(this RD9_THUNK_ARG, value);
}

float LightManager::getShadowIntensityBias() const
{
    return lm_getShadowIntensityBias(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setLightMapIntensityBias(float value)
{
    lm_setLightMapIntensityBias(this RD9_THUNK_ARG, value);
}

float LightManager::getLightMapIntensityBias() const
{
    return lm_getLightMapIntensityBias(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setDynamicShadowsEnabled(bool enabled)
{
    lm_setDynamicShadowsEnabled(this RD9_THUNK_ARG, enabled);
}

bool LightManager::getDynamicShadowsEnabled() const
{
    return lm_getDynamicShadowsEnabled(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setShadowUpdatesEnabled(bool enabled)
{
    lm_setShadowUpdatesEnabled(this RD9_THUNK_ARG, enabled);
}

bool LightManager::getShadowUpdatesEnabled() const
{
    return lm_getShadowUpdatesEnabled(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setShadowTSMFocus(float value)
{
    lm_setShadowTSMFocus(this RD9_THUNK_ARG, value);
}

float LightManager::getShadowTSMFocus() const
{
    return lm_getShadowTSMFocus(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::renderShadowMaps()
{
    lm_renderShadowMaps(this RD9_THUNK_ARG);
}

void LightManager::setSunDirection(Vec3 direction)
{
    lm_setSunDirection(this RD9_THUNK_ARG, direction);
}

Vec3 LightManager::getSunDirection() const
{
    Vec3 result;
    lm_getSunDirection(const_cast<LightManager*>(this) RD9_THUNK_ARG, &result);
    return result;
}

void LightManager::setSunColor(Vec3 color)
{
    lm_setSunColor(this RD9_THUNK_ARG, color);
}

Vec3 LightManager::getSunColor() const
{
    Vec3 result;
    lm_getSunColor(const_cast<LightManager*>(this) RD9_THUNK_ARG, &result);
    return result;
}

void LightManager::setSunSpecColor(Vec3 color)
{
    lm_setSunSpecColor(this RD9_THUNK_ARG, color);
}

Vec3 LightManager::getSunSpecColor() const
{
    Vec3 result;
    lm_getSunSpecColor(const_cast<LightManager*>(this) RD9_THUNK_ARG, &result);
    return result;
}

void LightManager::setSunEnabled(bool enabled)
{
    lm_setSunEnabled(this RD9_THUNK_ARG, enabled);
}

bool LightManager::getSunEnabled() const
{
    return lm_getSunEnabled(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setDefaultEffectLightAffectionFactor(float factor)
{
    lm_setDefaultEffectFactor(this RD9_THUNK_ARG, factor);
}

float LightManager::getDefaultEffectLightAffectionFactor() const
{
    return lm_getDefaultEffectFactor(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setShadowUseSweepTest(bool enabled)
{
    lm_setShadowUseSweepTest(this RD9_THUNK_ARG, enabled);
}

bool LightManager::getShadowUseSweepTest() const
{
    return lm_getShadowUseSweepTest(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setDynamicLightRangeScale(float scale)
{
    lm_setDynamicLightRangeScale(this RD9_THUNK_ARG, scale);
}

float LightManager::getDynamicLightRangeScale() const
{
    return lm_getDynamicLightRangeScale(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::setOccludedSkyNightIntensity(float intensity)
{
    lm_setOccludedSkyNightIntensity(this RD9_THUNK_ARG, intensity);
}

float LightManager::getOccludedSkyNightIntensity() const
{
    return lm_getOccludedSkyNightIntensity(const_cast<LightManager*>(this) RD9_THUNK_ARG);
}

void LightManager::registerShadowEffect(int reflectionTypeId, dx9rend::Effect* effect)
{
    lm_registerShadowEffect(this RD9_THUNK_ARG, reflectionTypeId, effect);
}

// The explicit host table requires plain x86 entry points. Keeping these adapters nested under
// LightManager makes the boundary unmistakable; every adapter immediately enters the corresponding
// qualified member body and none performs a vtable lookup.
struct LightManager::HostAbi
{
    static LightManager& object(void* self) { return *static_cast<LightManager*>(self); }

    static void RD9_THUNK addRef(void* self RD9_THUNK_EDX) { object(self).LightManager::addRef(); }
    static int RD9_THUNK getRef(void* self RD9_THUNK_EDX) { return object(self).LightManager::getRef(); }
    static int RD9_THUNK release(void* self RD9_THUNK_EDX) { return object(self).LightManager::release(); }
    static void* RD9_THUNK queryInterface(void* self RD9_THUNK_EDX, unsigned int iid)
    { return object(self).LightManager::queryInterface(iid); }
    static void RD9_THUNK update(void* self RD9_THUNK_EDX, double time)
    { object(self).LightManager::update(time); }
    static void RD9_THUNK getLightsByMask(void* self RD9_THUNK_EDX,
                                           std::vector<world::LightSource*>* out,
                                           unsigned int required, unsigned int forbidden)
    { object(self).LightManager::getLightsByMask(*out, required, forbidden); }
    static void RD9_THUNK getVisibleLightsByMask(void* self RD9_THUNK_EDX,
                                                  std::vector<world::LightSource*>* out,
                                                  unsigned int required, unsigned int forbidden)
    { object(self).LightManager::getVisibleLightsByMask(*out, required, forbidden); }
    static int RD9_THUNK getStaticMeshShadowLod(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getStaticMeshShadowLod(); }
    static void RD9_THUNK setBundledMeshShadowLod(void* self RD9_THUNK_EDX, int lod)
    { object(self).LightManager::setBundledMeshShadowLod(lod); }
    static int RD9_THUNK getBundledMeshShadowLod(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getBundledMeshShadowLod(); }
    static int RD9_THUNK getShadowMergeLodLimit(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getShadowMergeLodLimit(); }
    static void RD9_THUNK gatherLights(void* self RD9_THUNK_EDX, const Vec3* position, float radius,
                                        std::vector<world::LightSource*>* out,
                                        unsigned int maxLights,
                                        unsigned int required, unsigned int forbidden,
                                        unsigned char occlusionTest)
    { object(self).LightManager::gatherLights(*position, radius, *out, maxLights, required,
                                              forbidden, occlusionTest != 0); }
    // The virtual's last argument is a bool. x86 reads the whole stack dword the retail
    // `retn 18h` body pops; an x64 caller stores only the byte, so x64 reads only the bool.
    // [CHOSEN, not recovered] x64 spelling (https://github.com/B2BF/OpenBF2/issues/7).
#if defined(_M_IX86)
    typedef int GatherOcclusionArg;
#else
    typedef bool GatherOcclusionArg;
#endif
    static void RD9_THUNK gatherLightsForGeometry(void* self RD9_THUNK_EDX, void* geometry,
                                                   std::vector<world::LightSource*>* out,
                                                   unsigned int maxLights, unsigned int required,
                                                   unsigned int forbidden,
                                                   GatherOcclusionArg occlusionTest)
    { object(self).LightManager::gatherLightsForGeometry(geometry, *out, maxLights, required,
                                                         forbidden, occlusionTest != 0); }
    static void RD9_THUNK setSkyLight(void* self RD9_THUNK_EDX, world::LightSource* light)
    { object(self).LightManager::setSkyLight(light); }
    static world::LightSource* RD9_THUNK getSkyLight(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getSkyLight(); }

#define BF2_LIGHT_HOST_COLOR(Name)                                                \
    static void RD9_THUNK set##Name(void* self RD9_THUNK_EDX, const Vec3* color)   \
    { object(self).LightManager::set##Name(*color); }                              \
    static const Vec3* RD9_THUNK get##Name(void* self RD9_THUNK_EDX)               \
    { return &object(self).LightManager::get##Name(); }

    BF2_LIGHT_HOST_COLOR(SkyColor)
    BF2_LIGHT_HOST_COLOR(AmbientColor)
    BF2_LIGHT_HOST_COLOR(TreeAmbientColor)
    BF2_LIGHT_HOST_COLOR(TreeSunColor)
    BF2_LIGHT_HOST_COLOR(TreeSkyColor)
    BF2_LIGHT_HOST_COLOR(StaticSunColor)
    BF2_LIGHT_HOST_COLOR(StaticSpecularColor)
    BF2_LIGHT_HOST_COLOR(StaticSkyColor)
    BF2_LIGHT_HOST_COLOR(SinglePointColor)
    BF2_LIGHT_HOST_COLOR(DynamicPointColor)
    BF2_LIGHT_HOST_COLOR(EffectSunColor)
    BF2_LIGHT_HOST_COLOR(EffectShadowColor)
    BF2_LIGHT_HOST_COLOR(SinglePointColorHigh)
    BF2_LIGHT_HOST_COLOR(SinglePointColorLow)
    BF2_LIGHT_HOST_COLOR(DynamicPointColorHigh)
    BF2_LIGHT_HOST_COLOR(DynamicPointColorLow)
    BF2_LIGHT_HOST_COLOR(SkyColorHigh)
    BF2_LIGHT_HOST_COLOR(SkyColorLow)
    BF2_LIGHT_HOST_COLOR(StaticSkyColorHigh)
    BF2_LIGHT_HOST_COLOR(StaticSkyColorLow)
    BF2_LIGHT_HOST_COLOR(SunColorHigh)
    BF2_LIGHT_HOST_COLOR(SunColorLow)
    BF2_LIGHT_HOST_COLOR(SunSpecColorHigh)
    BF2_LIGHT_HOST_COLOR(SunSpecColorLow)

#undef BF2_LIGHT_HOST_COLOR

    static void RD9_THUNK setGroundHemi(void* self RD9_THUNK_EDX, const LightManagerHostString* name)
    { object(self).LightManager::setGroundHemi(*name); }
    static const std::string* RD9_THUNK getGroundHemi(void* self RD9_THUNK_EDX)
    { return &object(self).LightManager::getGroundHemi(); }
    static void RD9_THUNK setHemiLerpBias(void* self RD9_THUNK_EDX, float bias)
    { object(self).LightManager::setHemiLerpBias(bias); }
    static float RD9_THUNK getHemiLerpBias(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getHemiLerpBias(); }
    static void RD9_THUNK setLightCullDistance(void* self RD9_THUNK_EDX, float value)
    { object(self).LightManager::setLightCullDistance(value); }
    static float RD9_THUNK getLightCullDistance(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getLightCullDistance(); }

#define BF2_LIGHT_HOST_INT_PROPERTY(Name)                                         \
    static void RD9_THUNK set##Name(void* self RD9_THUNK_EDX, int value)           \
    { object(self).LightManager::set##Name(value); }                                \
    static int RD9_THUNK get##Name(void* self RD9_THUNK_EDX)                        \
    { return object(self).LightManager::get##Name(); }

    BF2_LIGHT_HOST_INT_PROPERTY(MaxLightsPerBundledMesh)
    BF2_LIGHT_HOST_INT_PROPERTY(MaxLightsPerStaticMesh)
    BF2_LIGHT_HOST_INT_PROPERTY(MaxLightsPerSkinnedMesh)
    BF2_LIGHT_HOST_INT_PROPERTY(MaxLightsPerTerrainPatch)
    BF2_LIGHT_HOST_INT_PROPERTY(SkinnedMeshShadowLod)
    BF2_LIGHT_HOST_INT_PROPERTY(TerrainShadowLod)
    BF2_LIGHT_HOST_INT_PROPERTY(ShadowMapSize)
    BF2_LIGHT_HOST_INT_PROPERTY(ShadowMapObjectSize)
    BF2_LIGHT_HOST_INT_PROPERTY(ShadowMapBitDepth)
    BF2_LIGHT_HOST_INT_PROPERTY(ShadowOverrideQuality)

    static LightDistanceMap* RD9_THUNK getVisibleLightsByDistance(
        void* self RD9_THUNK_EDX, unsigned int required, unsigned int forbidden)
    { return object(self).LightManager::getVisibleLightsByDistance(required, forbidden); }

    static void RD9_THUNK setStaticMeshShadowLod(void* self RD9_THUNK_EDX, int value)
    { object(self).LightManager::setStaticMeshShadowLod(value); }
    static void RD9_THUNK setShadowMergeLodLimit(void* self RD9_THUNK_EDX, int value)
    { object(self).LightManager::setShadowMergeLodLimit(value); }

#undef BF2_LIGHT_HOST_INT_PROPERTY

    static void RD9_THUNK addLight(void* self RD9_THUNK_EDX, world::LightSource* light)
    { object(self).LightManager::addLight(light); }
    static void RD9_THUNK removeLight(void* self RD9_THUNK_EDX, world::LightSource* light)
    { object(self).LightManager::removeLight(light); }
    static void RD9_THUNK enableLight(void* self RD9_THUNK_EDX, world::LightSource* light)
    { object(self).LightManager::enableLight(light); }
    static void RD9_THUNK disableLight(void* self RD9_THUNK_EDX, world::LightSource* light)
    { object(self).LightManager::disableLight(light); }

#define BF2_LIGHT_HOST_BOOL_PROPERTY(Name)                                        \
    static void RD9_THUNK set##Name(void* self RD9_THUNK_EDX, bool value)          \
    { object(self).LightManager::set##Name(value); }                                \
    static bool RD9_THUNK get##Name(void* self RD9_THUNK_EDX)                       \
    { return object(self).LightManager::get##Name(); }

    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowStaticFeature)
    BF2_LIGHT_HOST_BOOL_PROPERTY(DynamicShadowsEnabled)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowUpdatesEnabled)
    BF2_LIGHT_HOST_BOOL_PROPERTY(StencilCullerResetEnabled)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowUseSweepTest)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowShowFrustums)
    BF2_LIGHT_HOST_BOOL_PROPERTY(DynamicStaticMeshShadowsEnabled)
    BF2_LIGHT_HOST_BOOL_PROPERTY(Dynamic1pShadowsEnabled)
    BF2_LIGHT_HOST_BOOL_PROPERTY(AllowAlphaShadows)
    BF2_LIGHT_HOST_BOOL_PROPERTY(UseLSSMOptimizations)
    BF2_LIGHT_HOST_BOOL_PROPERTY(UsePSMOptimizations)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowPSMUseSlideback)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowPSMUseUnitCubeClipping)
    BF2_LIGHT_HOST_BOOL_PROPERTY(ShadowMergeDynamicShadowFrustums)

#undef BF2_LIGHT_HOST_BOOL_PROPERTY

#define BF2_LIGHT_HOST_FLOAT_PROPERTY(Name)                                       \
    static void RD9_THUNK set##Name(void* self RD9_THUNK_EDX, float value)         \
    { object(self).LightManager::set##Name(value); }                                \
    static float RD9_THUNK get##Name(void* self RD9_THUNK_EDX)                      \
    { return object(self).LightManager::get##Name(); }

    BF2_LIGHT_HOST_FLOAT_PROPERTY(SkinnedMeshShaderLodDistance)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowRoofRayLength)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowSunScalePower)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowIntensityBias)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(LightMapIntensityBias)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowTSMFocus)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(DefaultEffectLightAffectionFactor)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(DynamicLightRangeScale)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(OccludedSkyNightIntensity)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowDistance)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(UnusedFloat218)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowDepthBias)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowSlopeScaleDepthBias)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowPullbackDistance)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(AlphaShadowThreshold)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(DropShadowClipHeightOffset)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowUpdateFactor)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowOccluderOversizeFactor)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowPSMZNearOffset)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowPSMZFarOffset)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowPSMZMinInfinityZ)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(ShadowPSMFovY)
    BF2_LIGHT_HOST_FLOAT_PROPERTY(Shadow80Line)

#undef BF2_LIGHT_HOST_FLOAT_PROPERTY

    static const Vec2* RD9_THUNK getLightMapAmbientAlphaCutoffRange(void* self RD9_THUNK_EDX)
    { return &object(self).LightManager::getLightMapAmbientAlphaCutoffRange(); }
    static void RD9_THUNK setLightMapAmbientAlphaCutoffRange(void* self RD9_THUNK_EDX, const Vec2* range)
    { object(self).LightManager::setLightMapAmbientAlphaCutoffRange(*range); }
    static void RD9_THUNK setShadowSunHeightOverFade(void* self RD9_THUNK_EDX, const Vec2* range)
    { object(self).LightManager::setShadowSunHeightOverFade(*range); }
    static const Vec2* RD9_THUNK getShadowSunHeightOverFade(void* self RD9_THUNK_EDX)
    { return &object(self).LightManager::getShadowSunHeightOverFade(); }
    static float RD9_THUNK sampleSimpleShadowMap(void* self RD9_THUNK_EDX, float x, float z)
    { return object(self).LightManager::sampleSimpleShadowMap(x, z); }
    static float RD9_THUNK sampleSimpleShadowMapIO(void* self RD9_THUNK_EDX, float x, float z)
    { return object(self).LightManager::sampleSimpleShadowMapIO(x, z); }
    static float RD9_THUNK sampleSimpleShadowMapIOTexel(void* self RD9_THUNK_EDX, int x, int z)
    { return object(self).LightManager::sampleSimpleShadowMapIOTexel(x, z); }
    static float RD9_THUNK sampleSimpleShadowMapTexel(void* self RD9_THUNK_EDX, int x, int z)
    { return object(self).LightManager::sampleSimpleShadowMapTexel(x, z); }

    static void RD9_THUNK renderShadowMaps(void* self RD9_THUNK_EDX)
    { object(self).LightManager::renderShadowMaps(); }
    static void RD9_THUNK setSunDirection(void* self RD9_THUNK_EDX, Vec3 value)
    { object(self).LightManager::setSunDirection(value); }
    static Vec3* RD9_THUNK getSunDirection(void* self RD9_THUNK_EDX, Vec3* out)
    { *out = object(self).LightManager::getSunDirection(); return out; }
    static void RD9_THUNK setSunColor(void* self RD9_THUNK_EDX, Vec3 value)
    { object(self).LightManager::setSunColor(value); }
    static Vec3* RD9_THUNK getSunColor(void* self RD9_THUNK_EDX, Vec3* out)
    { *out = object(self).LightManager::getSunColor(); return out; }
    static void RD9_THUNK setSunSpecColor(void* self RD9_THUNK_EDX, Vec3 value)
    { object(self).LightManager::setSunSpecColor(value); }
    static Vec3* RD9_THUNK getSunSpecColor(void* self RD9_THUNK_EDX, Vec3* out)
    { *out = object(self).LightManager::getSunSpecColor(); return out; }
    static void RD9_THUNK setSunEnabled(void* self RD9_THUNK_EDX, bool enabled)
    { object(self).LightManager::setSunEnabled(enabled); }
    static bool RD9_THUNK getSunEnabled(void* self RD9_THUNK_EDX)
    { return object(self).LightManager::getSunEnabled(); }
    static void RD9_THUNK registerShadowEffect(void* self RD9_THUNK_EDX, int reflectionTypeId, dx9rend::Effect* effect)
    { object(self).LightManager::registerShadowEffect(reflectionTypeId, effect); }
    static void RD9_THUNK loadSimpleShadowMaps(void* self RD9_THUNK_EDX)
    { object(self).LightManager::loadSimpleShadowMaps(); }
};

namespace {

// PE 0x100F3310.
void RD9_THUNK lm_renderPathActivated(void* self RD9_THUNK_EDX, int path)
{
    void* const base = lmFromRenderPathListener(self);
    if (path == 2)
    {
        void* const effect = dx9rend::shaderManagerGetEffect(
            dx9rend::g_shaderManager, "Shaders/LightManager_r3x0");
        lm(base).shadowEffect = static_cast<dx9rend::Effect*>(effect);
        struct ParameterSpec { LightShaderParameter index; const char* semantic; };
        static const ParameterSpec parameters[] =
        {
            {kParamStencilFunc, "STENCILFUNC"}, {kParamStencilRef, "STENCILREF"},
            {kParamStencilPass, "STENCILPASS"},
            {kParamParaboloidZValues, "PARABOLOIDZVALUES"}, {kParamLightDir, "LIGHTDIR"},
            {kParamLightWorldPos, "LIGHTWORLDPOS"}, {kParamLightPos, "LIGHTPOS"},
            {kParamLightCol, "LIGHTCOL"},
            {kParamLightAttenuationRange, "LIGHTATTENUATIONRANGE"},
            {kParamLightAttenuationRangeInv, "LIGHTATTENUATIONRANGEINV"},
            {kParamProjectorMask, "PROJECTORMASK"}, {kParamEyePos, "EYEPOS"},
            {kParamEyeDof, "EYEDOF"},
            {kParamViewProj, "VIEWPROJ"}, {kParamWorldViewProj, "WORLDVIEWPROJ"},
            {kParamLightViewProj, "LIGHTVIEWPROJ"},
            {kParamLightOccluderViewProj, "LIGHTOCCLUDERVIEWPROJ"},
            {kParamViewportMap, "VIEWPORTMAP"}, {kParamViewportMap2, "VIEWPORTMAP2"},
            {kParamShadowIntensityBias, "SHADOWINTENSITYBIAS"},
            {kParamLightMapIntensityBias, "LIGHTMAPINTENSITYBIAS"},
            {kParamTexLayer0, "TEXLAYER0"}, {kParamTexLayer1, "TEXLAYER1"},
            {kParamTexLayer2, "TEXLAYER2"}, {kParamTexLayer3, "TEXLAYER3"},
            {kParamTexLayer4, "TEXLAYER4"}, {kParamTexLayer5, "TEXLAYER5"},
            {kParamTexLayer6, "TEXLAYER6"}
        };
        for (unsigned int i = 0; i < sizeof(parameters) / sizeof(parameters[0]); ++i)
            lm(base).shaderParameters[parameters[i].index] = static_cast<dx9rend::ShaderParameter*>(
                getEffectParameter(effect, parameters[i].semantic));
        static_cast<LightManager*>(base)->setShadowIntensityBias(0.0f);
        static_cast<LightManager*>(base)->setLightMapIntensityBias(0.0f);
    }
    lm(base).radialAttenuation = loadManagerTexture("Common/Textures/RadialAtt2");
    lm(base).conicalAttenuation = loadManagerTexture("Common/Textures/ConicalAtt");
}
void RD9_THUNK lm_renderPathNull04(void* RD9_THUNK_EDX) {}
void RD9_THUNK lm_renderPathNull08(void* RD9_THUNK_EDX, int) {}
#if !defined(_MSC_VER)
// [CHOSEN, not recovered] Match RenderPathListener's notification and integer carrier.
void lm_renderPathNull0C(void*, int) {}
int lm_getRenderPath(void* self)
{
    return static_cast<int>(reinterpret_cast<std::size_t>(lm(lmFromRenderPathListener(self)).renderPathValue));
}
#else
void RD9_THUNK lm_renderPathNull0C(void* RD9_THUNK_EDX, void*) {}
void* RD9_THUNK lm_getRenderPath(void* self RD9_THUNK_EDX)
{
    // PE 0x101A9A10 returns secondaryThis[1].  The render-path listener subobject starts at
    // base+0x08, so this is the pointer at base+0x0C (zero in the partial constructor).
    return lm(lmFromRenderPathListener(self)).renderPathValue;
}
#endif
// PE 0x100F7910.
renddx9abi::HostString* RD9_THUNK lm_getRenderPathName(void* RD9_THUNK_EDX, renddx9abi::HostString* out)
{
    // PE 0x100F7910 constructs this exact string into the caller's MSVC x86 sret storage and
    // returns it in EAX (`retn 4`). HostString selects the native or VC7.1 representation at the
    // plugin boundary without changing that physical call shape.
    new (out) renddx9abi::HostString("aLightManager");
    return out;
}

#if !defined(_MSC_VER)
// [CHOSEN, not recovered] Match the shared consumer interface under the native ABI.
const void* const kRenderPathVtable[] =
{
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, onRenderPathActivated, lm_renderPathActivated),
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, onRenderPathDeactivated, lm_renderPathNull04),
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, onRenderPathChanged, lm_renderPathNull08),
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, onRenderPathNotification, lm_renderPathNull0C),
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, getCurrentRenderPath, lm_getRenderPath),
    BF2_NATIVE_SLOT(::dice::hfe::dx9rend::RenderPathListener, getRenderPathListenerName, lm_getRenderPathName),
};
#else
const void* const kRenderPathVtable[6] =
{
    reinterpret_cast<void*>(&lm_renderPathActivated),
    reinterpret_cast<void*>(&lm_renderPathNull04),
    reinterpret_cast<void*>(&lm_renderPathNull08),
    reinterpret_cast<void*>(&lm_renderPathNull0C),
    reinterpret_cast<void*>(&lm_getRenderPath),
    reinterpret_cast<void*>(&lm_getRenderPathName)
};
#endif

// PE vtable 0x1021AAF0: 189 entries, +0x000..+0x2F0.
#if !defined(_MSC_VER)
// [CHOSEN, not recovered] Match the shared consumer interface under the native ABI.
const void* const kPrimaryVtable[] =
{
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, addRef, LightManager::HostAbi::addRef),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getRef, LightManager::HostAbi::getRef),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, release, LightManager::HostAbi::release),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, queryInterface, LightManager::HostAbi::queryInterface),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, update, LightManager::HostAbi::update),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getLightsByMask, LightManager::HostAbi::getLightsByMask),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getVisibleLightsByMask, LightManager::HostAbi::getVisibleLightsByMask),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, gatherLights, LightManager::HostAbi::gatherLights),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, gatherLightsForGeometry, LightManager::HostAbi::gatherLightsForGeometry),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkyLight, LightManager::HostAbi::setSkyLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkyLight, LightManager::HostAbi::getSkyLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkyColor, LightManager::HostAbi::setSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkyColor, LightManager::HostAbi::getSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setAmbientColor, LightManager::HostAbi::setAmbientColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getAmbientColor, LightManager::HostAbi::getAmbientColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setTreeAmbientColor, LightManager::HostAbi::setTreeAmbientColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getTreeAmbientColor, LightManager::HostAbi::getTreeAmbientColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setGroundHemi, LightManager::HostAbi::setGroundHemi),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getGroundHemi, LightManager::HostAbi::getGroundHemi),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setHemiLerpBias, LightManager::HostAbi::setHemiLerpBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getHemiLerpBias, LightManager::HostAbi::getHemiLerpBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setLightCullDistance, LightManager::HostAbi::setLightCullDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getLightCullDistance, LightManager::HostAbi::getLightCullDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setMaxLightsPerBundledMesh, LightManager::HostAbi::setMaxLightsPerBundledMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getMaxLightsPerBundledMesh, LightManager::HostAbi::getMaxLightsPerBundledMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setMaxLightsPerStaticMesh, LightManager::HostAbi::setMaxLightsPerStaticMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getMaxLightsPerStaticMesh, LightManager::HostAbi::getMaxLightsPerStaticMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setMaxLightsPerSkinnedMesh, LightManager::HostAbi::setMaxLightsPerSkinnedMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getMaxLightsPerSkinnedMesh, LightManager::HostAbi::getMaxLightsPerSkinnedMesh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setMaxLightsPerTerrainPatch, LightManager::HostAbi::setMaxLightsPerTerrainPatch),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getMaxLightsPerTerrainPatch, LightManager::HostAbi::getMaxLightsPerTerrainPatch),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkinnedMeshShaderLodDistance, LightManager::HostAbi::setSkinnedMeshShaderLodDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkinnedMeshShaderLodDistance, LightManager::HostAbi::getSkinnedMeshShaderLodDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getLightMapAmbientAlphaCutoffRange, LightManager::HostAbi::getLightMapAmbientAlphaCutoffRange),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setLightMapAmbientAlphaCutoffRange, LightManager::HostAbi::setLightMapAmbientAlphaCutoffRange),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, addLight, LightManager::HostAbi::addLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, removeLight, LightManager::HostAbi::removeLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, enableLight, LightManager::HostAbi::enableLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, disableLight, LightManager::HostAbi::disableLight),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowMapSize, LightManager::HostAbi::setShadowMapSize),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowMapSize, LightManager::HostAbi::getShadowMapSize),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowMapObjectSize, LightManager::HostAbi::setShadowMapObjectSize),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowMapObjectSize, LightManager::HostAbi::getShadowMapObjectSize),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowMapBitDepth, LightManager::HostAbi::setShadowMapBitDepth),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowMapBitDepth, LightManager::HostAbi::getShadowMapBitDepth),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowDistance, LightManager::HostAbi::setShadowDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowDistance, LightManager::HostAbi::getShadowDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setUnusedFloat218, LightManager::HostAbi::setUnusedFloat218),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getUnusedFloat218, LightManager::HostAbi::getUnusedFloat218),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowStaticFeature, LightManager::HostAbi::setShadowStaticFeature),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowStaticFeature, LightManager::HostAbi::getShadowStaticFeature),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowShowFrustums, LightManager::HostAbi::setShadowShowFrustums),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowShowFrustums, LightManager::HostAbi::getShadowShowFrustums),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowIntensityBias, LightManager::HostAbi::setShadowIntensityBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowIntensityBias, LightManager::HostAbi::getShadowIntensityBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setLightMapIntensityBias, LightManager::HostAbi::setLightMapIntensityBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getLightMapIntensityBias, LightManager::HostAbi::getLightMapIntensityBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDropShadowClipHeightOffset, LightManager::HostAbi::setDropShadowClipHeightOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDropShadowClipHeightOffset, LightManager::HostAbi::getDropShadowClipHeightOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicShadowsEnabled, LightManager::HostAbi::setDynamicShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicShadowsEnabled, LightManager::HostAbi::getDynamicShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicStaticMeshShadowsEnabled, LightManager::HostAbi::setDynamicStaticMeshShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicStaticMeshShadowsEnabled, LightManager::HostAbi::getDynamicStaticMeshShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamic1pShadowsEnabled, LightManager::HostAbi::setDynamic1pShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamic1pShadowsEnabled, LightManager::HostAbi::getDynamic1pShadowsEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setAllowAlphaShadows, LightManager::HostAbi::setAllowAlphaShadows),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getAllowAlphaShadows, LightManager::HostAbi::getAllowAlphaShadows),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setAlphaShadowThreshold, LightManager::HostAbi::setAlphaShadowThreshold),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getAlphaShadowThreshold, LightManager::HostAbi::getAlphaShadowThreshold),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowUpdateFactor, LightManager::HostAbi::setShadowUpdateFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowUpdateFactor, LightManager::HostAbi::getShadowUpdateFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStencilCullerResetEnabled, LightManager::HostAbi::setStencilCullerResetEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStencilCullerResetEnabled, LightManager::HostAbi::getStencilCullerResetEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowUpdatesEnabled, LightManager::HostAbi::setShadowUpdatesEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowUpdatesEnabled, LightManager::HostAbi::getShadowUpdatesEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowDepthBias, LightManager::HostAbi::setShadowDepthBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowDepthBias, LightManager::HostAbi::getShadowDepthBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowSlopeScaleDepthBias, LightManager::HostAbi::setShadowSlopeScaleDepthBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowSlopeScaleDepthBias, LightManager::HostAbi::getShadowSlopeScaleDepthBias),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowOccluderOversizeFactor, LightManager::HostAbi::getShadowOccluderOversizeFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowOccluderOversizeFactor, LightManager::HostAbi::setShadowOccluderOversizeFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getUseLSSMOptimizations, LightManager::HostAbi::getUseLSSMOptimizations),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setUseLSSMOptimizations, LightManager::HostAbi::setUseLSSMOptimizations),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getUsePSMOptimizations, LightManager::HostAbi::getUsePSMOptimizations),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setUsePSMOptimizations, LightManager::HostAbi::setUsePSMOptimizations),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMUseSlideback, LightManager::HostAbi::getShadowPSMUseSlideback),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMUseSlideback, LightManager::HostAbi::setShadowPSMUseSlideback),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMUseUnitCubeClipping, LightManager::HostAbi::getShadowPSMUseUnitCubeClipping),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMUseUnitCubeClipping, LightManager::HostAbi::setShadowPSMUseUnitCubeClipping),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMZNearOffset, LightManager::HostAbi::getShadowPSMZNearOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMZNearOffset, LightManager::HostAbi::setShadowPSMZNearOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMZFarOffset, LightManager::HostAbi::getShadowPSMZFarOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMZFarOffset, LightManager::HostAbi::setShadowPSMZFarOffset),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMZMinInfinityZ, LightManager::HostAbi::getShadowPSMZMinInfinityZ),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMZMinInfinityZ, LightManager::HostAbi::setShadowPSMZMinInfinityZ),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPSMFovY, LightManager::HostAbi::getShadowPSMFovY),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPSMFovY, LightManager::HostAbi::setShadowPSMFovY),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowTSMFocus, LightManager::HostAbi::getShadowTSMFocus),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowTSMFocus, LightManager::HostAbi::setShadowTSMFocus),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadow80Line, LightManager::HostAbi::getShadow80Line),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadow80Line, LightManager::HostAbi::setShadow80Line),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowMergeDynamicShadowFrustums, LightManager::HostAbi::setShadowMergeDynamicShadowFrustums),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowMergeDynamicShadowFrustums, LightManager::HostAbi::getShadowMergeDynamicShadowFrustums),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, renderShadowMaps, LightManager::HostAbi::renderShadowMaps),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunDirection, LightManager::HostAbi::setSunDirection),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunDirection, LightManager::HostAbi::getSunDirection),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunColor, LightManager::HostAbi::setSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunColor, LightManager::HostAbi::getSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunSpecColor, LightManager::HostAbi::setSunSpecColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunSpecColor, LightManager::HostAbi::getSunSpecColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunEnabled, LightManager::HostAbi::setSunEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunEnabled, LightManager::HostAbi::getSunEnabled),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getVisibleLightsByDistance, LightManager::HostAbi::getVisibleLightsByDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setTreeSunColor, LightManager::HostAbi::setTreeSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getTreeSunColor, LightManager::HostAbi::getTreeSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setTreeSkyColor, LightManager::HostAbi::setTreeSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getTreeSkyColor, LightManager::HostAbi::getTreeSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticSunColor, LightManager::HostAbi::setStaticSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticSunColor, LightManager::HostAbi::getStaticSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticSpecularColor, LightManager::HostAbi::setStaticSpecularColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticSpecularColor, LightManager::HostAbi::getStaticSpecularColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticSkyColor, LightManager::HostAbi::setStaticSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticSkyColor, LightManager::HostAbi::getStaticSkyColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSinglePointColor, LightManager::HostAbi::setSinglePointColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSinglePointColor, LightManager::HostAbi::getSinglePointColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicPointColor, LightManager::HostAbi::setDynamicPointColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicPointColor, LightManager::HostAbi::getDynamicPointColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSinglePointColorHigh, LightManager::HostAbi::setSinglePointColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSinglePointColorHigh, LightManager::HostAbi::getSinglePointColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSinglePointColorLow, LightManager::HostAbi::setSinglePointColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSinglePointColorLow, LightManager::HostAbi::getSinglePointColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicPointColorHigh, LightManager::HostAbi::setDynamicPointColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicPointColorHigh, LightManager::HostAbi::getDynamicPointColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicPointColorLow, LightManager::HostAbi::setDynamicPointColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicPointColorLow, LightManager::HostAbi::getDynamicPointColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkyColorHigh, LightManager::HostAbi::setSkyColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkyColorHigh, LightManager::HostAbi::getSkyColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkyColorLow, LightManager::HostAbi::setSkyColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkyColorLow, LightManager::HostAbi::getSkyColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticSkyColorHigh, LightManager::HostAbi::setStaticSkyColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticSkyColorHigh, LightManager::HostAbi::getStaticSkyColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticSkyColorLow, LightManager::HostAbi::setStaticSkyColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticSkyColorLow, LightManager::HostAbi::getStaticSkyColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunColorHigh, LightManager::HostAbi::setSunColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunColorHigh, LightManager::HostAbi::getSunColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunColorLow, LightManager::HostAbi::setSunColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunColorLow, LightManager::HostAbi::getSunColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunSpecColorHigh, LightManager::HostAbi::setSunSpecColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunSpecColorHigh, LightManager::HostAbi::getSunSpecColorHigh),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSunSpecColorLow, LightManager::HostAbi::setSunSpecColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSunSpecColorLow, LightManager::HostAbi::getSunSpecColorLow),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setEffectSunColor, LightManager::HostAbi::setEffectSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getEffectSunColor, LightManager::HostAbi::getEffectSunColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setEffectShadowColor, LightManager::HostAbi::setEffectShadowColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getEffectShadowColor, LightManager::HostAbi::getEffectShadowColor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDefaultEffectLightAffectionFactor, LightManager::HostAbi::setDefaultEffectLightAffectionFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDefaultEffectLightAffectionFactor, LightManager::HostAbi::getDefaultEffectLightAffectionFactor),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setTerrainShadowLod, LightManager::HostAbi::setTerrainShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getTerrainShadowLod, LightManager::HostAbi::getTerrainShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setStaticMeshShadowLod, LightManager::HostAbi::setStaticMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getStaticMeshShadowLod, LightManager::HostAbi::getStaticMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setBundledMeshShadowLod, LightManager::HostAbi::setBundledMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getBundledMeshShadowLod, LightManager::HostAbi::getBundledMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setSkinnedMeshShadowLod, LightManager::HostAbi::setSkinnedMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getSkinnedMeshShadowLod, LightManager::HostAbi::getSkinnedMeshShadowLod),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowOverrideQuality, LightManager::HostAbi::setShadowOverrideQuality),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowOverrideQuality, LightManager::HostAbi::getShadowOverrideQuality),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowRoofRayLength, LightManager::HostAbi::setShadowRoofRayLength),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowRoofRayLength, LightManager::HostAbi::getShadowRoofRayLength),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowSunScalePower, LightManager::HostAbi::setShadowSunScalePower),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowSunScalePower, LightManager::HostAbi::getShadowSunScalePower),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowPullbackDistance, LightManager::HostAbi::setShadowPullbackDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowPullbackDistance, LightManager::HostAbi::getShadowPullbackDistance),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowUseSweepTest, LightManager::HostAbi::setShadowUseSweepTest),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowUseSweepTest, LightManager::HostAbi::getShadowUseSweepTest),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowSunHeightOverFade, LightManager::HostAbi::setShadowSunHeightOverFade),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowSunHeightOverFade, LightManager::HostAbi::getShadowSunHeightOverFade),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, sampleSimpleShadowMap, LightManager::HostAbi::sampleSimpleShadowMap),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, sampleSimpleShadowMapIO, LightManager::HostAbi::sampleSimpleShadowMapIO),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, sampleSimpleShadowMapIOTexel, LightManager::HostAbi::sampleSimpleShadowMapIOTexel),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, sampleSimpleShadowMapTexel, LightManager::HostAbi::sampleSimpleShadowMapTexel),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setShadowMergeLodLimit, LightManager::HostAbi::setShadowMergeLodLimit),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getShadowMergeLodLimit, LightManager::HostAbi::getShadowMergeLodLimit),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setDynamicLightRangeScale, LightManager::HostAbi::setDynamicLightRangeScale),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getDynamicLightRangeScale, LightManager::HostAbi::getDynamicLightRangeScale),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, setOccludedSkyNightIntensity, LightManager::HostAbi::setOccludedSkyNightIntensity),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, getOccludedSkyNightIntensity, LightManager::HostAbi::getOccludedSkyNightIntensity),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, loadSimpleShadowMaps, LightManager::HostAbi::loadSimpleShadowMaps),
    BF2_NATIVE_SLOT(::dice::hfe::geom::LightManager, registerShadowEffect, LightManager::HostAbi::registerShadowEffect),
};
#else
const void* const kPrimaryVtable[189] =
{
    reinterpret_cast<void*>(&LightManager::HostAbi::addRef), // +0x000
    reinterpret_cast<void*>(&LightManager::HostAbi::getRef), // +0x004
    reinterpret_cast<void*>(&LightManager::HostAbi::release), // +0x008
    reinterpret_cast<void*>(&LightManager::HostAbi::queryInterface), // +0x00C
    reinterpret_cast<void*>(&LightManager::HostAbi::update), // +0x010
    reinterpret_cast<void*>(&LightManager::HostAbi::getLightsByMask), // +0x014
    reinterpret_cast<void*>(&LightManager::HostAbi::getVisibleLightsByMask), // +0x018
    reinterpret_cast<void*>(&LightManager::HostAbi::gatherLights), // +0x01C
    reinterpret_cast<void*>(&LightManager::HostAbi::gatherLightsForGeometry), // +0x020
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkyLight), // +0x024
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkyLight), // +0x028
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkyColor), // +0x02C
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkyColor), // +0x030
    reinterpret_cast<void*>(&LightManager::HostAbi::setAmbientColor), // +0x034
    reinterpret_cast<void*>(&LightManager::HostAbi::getAmbientColor), // +0x038
    reinterpret_cast<void*>(&LightManager::HostAbi::setTreeAmbientColor), // +0x03C
    reinterpret_cast<void*>(&LightManager::HostAbi::getTreeAmbientColor), // +0x040
    reinterpret_cast<void*>(&LightManager::HostAbi::setGroundHemi), // +0x044
    reinterpret_cast<void*>(&LightManager::HostAbi::getGroundHemi), // +0x048
    reinterpret_cast<void*>(&LightManager::HostAbi::setHemiLerpBias), // +0x04C
    reinterpret_cast<void*>(&LightManager::HostAbi::getHemiLerpBias), // +0x050
    reinterpret_cast<void*>(&LightManager::HostAbi::setLightCullDistance), // +0x054
    reinterpret_cast<void*>(&LightManager::HostAbi::getLightCullDistance), // +0x058
    reinterpret_cast<void*>(&LightManager::HostAbi::setMaxLightsPerBundledMesh), // +0x05C
    reinterpret_cast<void*>(&LightManager::HostAbi::getMaxLightsPerBundledMesh), // +0x060
    reinterpret_cast<void*>(&LightManager::HostAbi::setMaxLightsPerStaticMesh), // +0x064
    reinterpret_cast<void*>(&LightManager::HostAbi::getMaxLightsPerStaticMesh), // +0x068
    reinterpret_cast<void*>(&LightManager::HostAbi::setMaxLightsPerSkinnedMesh), // +0x06C
    reinterpret_cast<void*>(&LightManager::HostAbi::getMaxLightsPerSkinnedMesh), // +0x070
    reinterpret_cast<void*>(&LightManager::HostAbi::setMaxLightsPerTerrainPatch), // +0x074
    reinterpret_cast<void*>(&LightManager::HostAbi::getMaxLightsPerTerrainPatch), // +0x078
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkinnedMeshShaderLodDistance), // +0x07C
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkinnedMeshShaderLodDistance), // +0x080
    reinterpret_cast<void*>(&LightManager::HostAbi::getLightMapAmbientAlphaCutoffRange), // +0x084
    reinterpret_cast<void*>(&LightManager::HostAbi::setLightMapAmbientAlphaCutoffRange), // +0x088
    reinterpret_cast<void*>(&LightManager::HostAbi::addLight), // +0x08C
    reinterpret_cast<void*>(&LightManager::HostAbi::removeLight), // +0x090
    reinterpret_cast<void*>(&LightManager::HostAbi::enableLight), // +0x094
    reinterpret_cast<void*>(&LightManager::HostAbi::disableLight), // +0x098
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowMapSize),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowMapSize),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowMapObjectSize),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowMapObjectSize),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowMapBitDepth),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowMapBitDepth),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowDistance),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowDistance),
    reinterpret_cast<void*>(&LightManager::HostAbi::setUnusedFloat218), // +0x0BC
    reinterpret_cast<void*>(&LightManager::HostAbi::getUnusedFloat218), // +0x0C0
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowStaticFeature), // +0x0C4
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowStaticFeature), // +0x0C8
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowShowFrustums),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowShowFrustums),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowIntensityBias), // +0x0D4
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowIntensityBias), // +0x0D8
    reinterpret_cast<void*>(&LightManager::HostAbi::setLightMapIntensityBias), // +0x0DC
    reinterpret_cast<void*>(&LightManager::HostAbi::getLightMapIntensityBias), // +0x0E0
    reinterpret_cast<void*>(&LightManager::HostAbi::setDropShadowClipHeightOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::getDropShadowClipHeightOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicShadowsEnabled), // +0x0EC
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicShadowsEnabled), // +0x0F0
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicStaticMeshShadowsEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicStaticMeshShadowsEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamic1pShadowsEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamic1pShadowsEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::setAllowAlphaShadows),
    reinterpret_cast<void*>(&LightManager::HostAbi::getAllowAlphaShadows),
    reinterpret_cast<void*>(&LightManager::HostAbi::setAlphaShadowThreshold),
    reinterpret_cast<void*>(&LightManager::HostAbi::getAlphaShadowThreshold),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowUpdateFactor),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowUpdateFactor),
    reinterpret_cast<void*>(&LightManager::HostAbi::setStencilCullerResetEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::getStencilCullerResetEnabled),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowUpdatesEnabled), // +0x124
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowUpdatesEnabled), // +0x128
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowDepthBias),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowDepthBias),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowSlopeScaleDepthBias),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowSlopeScaleDepthBias),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowOccluderOversizeFactor),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowOccluderOversizeFactor),
    reinterpret_cast<void*>(&LightManager::HostAbi::getUseLSSMOptimizations),
    reinterpret_cast<void*>(&LightManager::HostAbi::setUseLSSMOptimizations),
    reinterpret_cast<void*>(&LightManager::HostAbi::getUsePSMOptimizations),
    reinterpret_cast<void*>(&LightManager::HostAbi::setUsePSMOptimizations),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMUseSlideback),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMUseSlideback),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMUseUnitCubeClipping),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMUseUnitCubeClipping),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMZNearOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMZNearOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMZFarOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMZFarOffset),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMZMinInfinityZ),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMZMinInfinityZ),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPSMFovY),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPSMFovY),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowTSMFocus), // +0x184
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowTSMFocus),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadow80Line),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadow80Line),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowMergeDynamicShadowFrustums),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowMergeDynamicShadowFrustums),
    reinterpret_cast<void*>(&LightManager::HostAbi::renderShadowMaps), // +0x19C
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunDirection), // +0x1A0
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunDirection), // +0x1A4
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunColor), // +0x1A8
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunColor), // +0x1AC
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunSpecColor), // +0x1B0
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunSpecColor), // +0x1B4
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunEnabled), // +0x1B8
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunEnabled), // +0x1BC
    reinterpret_cast<void*>(&LightManager::HostAbi::getVisibleLightsByDistance),
    reinterpret_cast<void*>(&LightManager::HostAbi::setTreeSunColor), // +0x1C4
    reinterpret_cast<void*>(&LightManager::HostAbi::getTreeSunColor), // +0x1C8
    reinterpret_cast<void*>(&LightManager::HostAbi::setTreeSkyColor), // +0x1CC
    reinterpret_cast<void*>(&LightManager::HostAbi::getTreeSkyColor), // +0x1D0
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticSunColor), // +0x1D4
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticSunColor), // +0x1D8
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticSpecularColor), // +0x1DC
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticSpecularColor), // +0x1E0
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticSkyColor), // +0x1E4
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticSkyColor), // +0x1E8
    reinterpret_cast<void*>(&LightManager::HostAbi::setSinglePointColor), // +0x1EC
    reinterpret_cast<void*>(&LightManager::HostAbi::getSinglePointColor), // +0x1F0
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicPointColor), // +0x1F4
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicPointColor), // +0x1F8
    reinterpret_cast<void*>(&LightManager::HostAbi::setSinglePointColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSinglePointColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSinglePointColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSinglePointColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicPointColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicPointColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicPointColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicPointColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkyColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkyColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkyColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkyColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticSkyColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticSkyColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticSkyColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticSkyColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunSpecColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunSpecColorHigh),
    reinterpret_cast<void*>(&LightManager::HostAbi::setSunSpecColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::getSunSpecColorLow),
    reinterpret_cast<void*>(&LightManager::HostAbi::setEffectSunColor), // +0x25C
    reinterpret_cast<void*>(&LightManager::HostAbi::getEffectSunColor), // +0x260
    reinterpret_cast<void*>(&LightManager::HostAbi::setEffectShadowColor), // +0x264
    reinterpret_cast<void*>(&LightManager::HostAbi::getEffectShadowColor), // +0x268
    reinterpret_cast<void*>(&LightManager::HostAbi::setDefaultEffectLightAffectionFactor), // +0x26C
    reinterpret_cast<void*>(&LightManager::HostAbi::getDefaultEffectLightAffectionFactor), // +0x270
    reinterpret_cast<void*>(&LightManager::HostAbi::setTerrainShadowLod),
    reinterpret_cast<void*>(&LightManager::HostAbi::getTerrainShadowLod),
    reinterpret_cast<void*>(&LightManager::HostAbi::setStaticMeshShadowLod),
    reinterpret_cast<void*>(&LightManager::HostAbi::getStaticMeshShadowLod), // +0x280
    reinterpret_cast<void*>(&LightManager::HostAbi::setBundledMeshShadowLod), // +0x284
    reinterpret_cast<void*>(&LightManager::HostAbi::getBundledMeshShadowLod), // +0x288
    reinterpret_cast<void*>(&LightManager::HostAbi::setSkinnedMeshShadowLod), // +0x28C
    reinterpret_cast<void*>(&LightManager::HostAbi::getSkinnedMeshShadowLod), // +0x290
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowOverrideQuality),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowOverrideQuality),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowRoofRayLength), // +0x29C
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowRoofRayLength), // +0x2A0
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowSunScalePower), // +0x2A4
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowSunScalePower), // +0x2A8
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowPullbackDistance),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowPullbackDistance),
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowUseSweepTest), // +0x2B4
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowUseSweepTest), // +0x2B8
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowSunHeightOverFade), // +0x2BC
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowSunHeightOverFade), // +0x2C0
    reinterpret_cast<void*>(&LightManager::HostAbi::sampleSimpleShadowMap), // +0x2C4
    reinterpret_cast<void*>(&LightManager::HostAbi::sampleSimpleShadowMapIO), // +0x2C8
    reinterpret_cast<void*>(&LightManager::HostAbi::sampleSimpleShadowMapIOTexel), // +0x2CC
    reinterpret_cast<void*>(&LightManager::HostAbi::sampleSimpleShadowMapTexel), // +0x2D0
    reinterpret_cast<void*>(&LightManager::HostAbi::setShadowMergeLodLimit),
    reinterpret_cast<void*>(&LightManager::HostAbi::getShadowMergeLodLimit), // +0x2D8
    reinterpret_cast<void*>(&LightManager::HostAbi::setDynamicLightRangeScale), // +0x2DC
    reinterpret_cast<void*>(&LightManager::HostAbi::getDynamicLightRangeScale), // +0x2E0
    reinterpret_cast<void*>(&LightManager::HostAbi::setOccludedSkyNightIntensity), // +0x2E4
    reinterpret_cast<void*>(&LightManager::HostAbi::getOccludedSkyNightIntensity), // +0x2E8
    reinterpret_cast<void*>(&LightManager::HostAbi::loadSimpleShadowMaps), // +0x2EC
    reinterpret_cast<void*>(&LightManager::HostAbi::registerShadowEffect), // +0x2F0
};
#endif

// PE 0x100F4770.
void constructLightManager(void* self)
{
    // PE 0x100F4770 performs explicit member construction; it does not blanket-zero the allocation.
    // The containers are constructed first (LightManagerData's constructor); every other field is
    // written here in the PE's order.
    LightManagerData& d = *::new (self) LightManagerData;
    d.primaryVtable = kPrimaryVtable;
    d.renderPathVtable = kRenderPathVtable;
    d.refCount = 1;
    d.skyLight = 0;

    d.skyColor.x = 0.4f;
    d.skyColor.y = 0.4f;
    d.skyColor.z = 0.7f;
    d.treeSunColor.x = 1.0f;                                    // +0x24..+0x38 1.0
    d.treeSunColor.y = 1.0f;
    d.treeSunColor.z = 1.0f;
    d.treeSkyColor.x = 1.0f;
    d.treeSkyColor.y = 1.0f;
    d.treeSkyColor.z = 1.0f;
    d.ambientColor.x = 0.3f;                                    // +0x3C..+0x44 0.3
    d.ambientColor.y = 0.3f;
    d.ambientColor.z = 0.3f;
    d.treeAmbientColor.x = 0.0f;                                // +0x48..+0x50 0
    d.treeAmbientColor.y = 0.0f;
    d.treeAmbientColor.z = 0.0f;
    d.staticSunColor.x = 1.0f;
    d.staticSunColor.y = 0.5f;
    d.staticSunColor.z = 0.2f;
    d.staticSkyColor.x = 0.3f;
    d.staticSkyColor.y = 1.0f;
    d.staticSkyColor.z = 0.5f;
    d.staticSpecularColor.x = 1.0f;                             // +0x6C..+0x7C 1.0
    d.staticSpecularColor.y = 1.0f;
    d.staticSpecularColor.z = 1.0f;
    d.singlePointColor.x = 1.0f;
    d.singlePointColor.y = 1.0f;
    d.singlePointColor.z = 0.8f;
    d.dynamicPointColor.x = 1.0f;                               // +0x84..+0x8C 1.0
    d.dynamicPointColor.y = 1.0f;
    d.dynamicPointColor.z = 1.0f;
    for (unsigned int i = 0; i < 12; ++i)
    {
        d.blendedColors[i].x = 0.0f;
        d.blendedColors[i].y = 0.0f;
        d.blendedColors[i].z = 0.0f;
    }
    d.effectSunColor.x = 1.0f;                                  // +0x120..+0x134 1.0
    d.effectSunColor.y = 1.0f;
    d.effectSunColor.z = 1.0f;
    d.effectShadowColor.x = 1.0f;
    d.effectShadowColor.y = 1.0f;
    d.effectShadowColor.z = 1.0f;

    d.defaultEffectLightAffectionFactor = 1.0f;
    d.lightCullDistance = 300.0f;
    d.maxLightsPerBundledMesh = 3;
    d.maxLightsPerStaticMesh = 3;
    d.maxLightsPerSkinnedMesh = 3;
    d.maxLightsPerTerrainPatch = 7;
    d.terrainShadowLod = 1;
    d.staticMeshShadowLod = 1;
    d.bundledMeshShadowLod = 0;
    d.skinnedMeshShadowLod = 0;
    d.shadowOverrideQuality = -1;
    d.shadowRoofRayLength = 150.0f;
    d.shadowSunScalePower = 1.0f;
    d.shadowPullbackDistance = 200.0f;
    d.shadowUseSweepTest = 1;
    d.shadowSunHeightOverFade.x = 3.0f;
    d.shadowSunHeightOverFade.y = 6.0f;
    d.simpleShadowIOBitCount = 0;
    d.simpleShadowMap = 0;                                      // +0x190..+0x1B0 0
    d.simpleShadowLimit = 0.0f;
    d.simpleShadowIOStride = 0;
    d.simpleShadowIOColumnOrigin = 0;
    d.simpleShadowRowCount = 0;
    d.simpleShadowIORowOrigin = 0;
    d.simpleShadowStride = 0;
    d.simpleShadowColumnOrigin = 0;
    d.simpleShadowRowOrigin = 0;
    d.skinnedMeshShaderLodDistance = 10.0f;

    // +0x1B8..+0x1F0 - the two light sets (PE 0x100F5BC0's inlined constructors), the frame list,
    // +0x1E0 and the +0x1F0 tree - are constructed as members.
    d.lightMapAmbientAlphaCutoffRange.x = 0.1f;
    d.lightMapAmbientAlphaCutoffRange.y = 0.5f;
    d.shadowMapSize = 1024;
    d.shadowTileSize = 256;
    d.shadowDistance = 40.0f;
    d.unresolved218 = 40.0f;
    d.normalCubeTexture = 0;
    d.currentSpecularLut = 0;
    d.specularLutIndex = 0;
    d.shadowStaticFeature = 1;
    d.dropShadowClipHeightOffset = 0.0f;

    d.unresolvedMatrices648[0] = Mat4::getIdentity();
    d.unresolvedMatrices648[1] = Mat4::getIdentity();
    d.unresolvedMatrices648[2] = Mat4::getIdentity();
    d.unresolvedMatrices648[3] = Mat4::getIdentity();
    d.unresolvedMatrices648[4] = Mat4::getIdentity();
    d.showFrustums = 0;

    d.dynamicShadowsEnabled = 1;
    d.firstPersonShadows = 1;
    d.dynamicStaticMeshShadowsEnabled = 1;
    d.allowAlphaShadows = 0;
    d.alphaShadowThreshold = 0.2f;
    d.shadowUpdateFactor = 1.0f;
    d.stencilCullerResetEnabled = 1;
    d.shadowUpdatesEnabled = 1;
    d.shadowOccluderOversizeFactor = 1.0f;
    d.shadowProjectionFlags[0] = 0;
    d.shadowProjectionFlags[1] = 0;
    d.shadowProjectionFlags[2] = 1;
    d.shadowProjectionFlags[3] = 1;
    d.shadowPSMZNearOffset = 0.0f;
    d.shadowPSMZFarOffset = 0.0f;
    d.shadowPSMZMinInfinityZ = 1.5f;
    d.shadowPSMFovY = 60.0f;
    d.shadowTSMFocus = 0.52f;
    d.shadow80Line = -0.6f;
    d.shadowMergeDynamicShadowFrustums = 1;
    d.shadowMergeLodLimit = 16;
    d.dynamicLightRangeScale = 1.0f;
    d.occludedSkyNightIntensity = 0.0f;
    d.shadowDepthBias = 0.005f;
    d.shadowSlopeScaleDepthBias = 1.0f;
    d.shadowFadeDuration = 1.0;
    d.sunLight = 0;                                             // +0x840..+0x8BC 0
    d.shadowEffect = 0;
    for (unsigned int i = 0; i < kLightShaderParameterCount; ++i)
        d.shaderParameters[i] = 0;
    d.radialAttenuation = 0;
    d.conicalAttenuation = 0;

    d.sphereVertices = 0;                                       // +0x8D8..+0x904 0
    d.sphereIndices = 0;
    d.sphereVertexCount = 0;
    d.sphereFaceCount = 0;
    d.cylinderVertices = 0;
    d.cylinderIndices = 0;
    d.cylinderVertexCount = 0;
    d.cylinderFaceCount = 0;
    d.boxVertices = 0;
    d.boxIndices = 0;
    d.boxVertexCount = 0;
    d.boxFaceCount = 0;
    d.shadowMapMode = 2;

    IDirect3DDevice9* const device =
        static_cast<dx9rend::RendDX9Base*>(dx9rend::g_rendererBase)->device;   // +0x0C
    d.device = device;
    device->AddRef();

    const Mat4 identity = Mat4::getIdentity();
    rendmath::frustumSetOrtho(d.shadowFrustum, 1.0f, 1.0f, 0.0f, 1.0f, identity);
    d.unresolvedMatrices648[3] = Mat4::getIdentity();

    if (dx9rend::deviceCapsIsNvidia())
    {
        d.shadowMapMode = 0;
        d.shadowMapUsage = D3DUSAGE_DEPTHSTENCIL;
        d.shadowMapFormat = D3DFMT_D24X8;
    }
    else
    {
        d.shadowMapUsage = D3DUSAGE_RENDERTARGET;
        const unsigned int formats[] = { 111u, 114u, 112u };
        for (unsigned int i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
        {
            if (!dx9rend::deviceCapsSupportsTextureFormat(formats[i]))
                continue;
            d.shadowMapFormat = formats[i];
            d.shadowMapMode = 1;
            break;
        }
    }

    const Vec3 white = { 1.0f, 1.0f, 1.0f };
    d.specularLuts[0] = createPowerLut(256, white, 64.0f, false);
    d.specularLuts[1] = createPowerLut(256, white, 64.0f, false);
    d.specularLuts[2] = createPowerLut(256, white, 64.0f, false);
    d.specularLuts[3] = createPowerLut(256, white, 64.0f, false);
    d.dynamicShadowsEnabled = d.shadowMapMode != 2 ? 1 : 0;

    dx9rend::registerRenderPathListener(
        reinterpret_cast<dx9rend::RenderPathListener*>(&d.renderPathVtable));
    createSmallLightTextures(self);

    ::renddx9abi::logf("LightManager: built at 0x%08X (retail size 0x980); lifecycle, shadow "
                       "resources and outer shadow transaction recovered",
                       ::renddx9abi::addr(self));
}

} // anonymous namespace

namespace {
// [INFERRED] Source spelling of the inlined expansion-color arithmetic.
inline Vec3 blendExpansionColor(const Vec3& low, const Vec3& high, float blend,
                               float inverse, bool roundHighY)
{
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Preserve the x87 stores between vector operations.
    typedef bf2x87::Real Real;
    const float lowX = bf2x87::store(Real(low.x) * Real(blend));
    const float lowY = bf2x87::store(Real(low.y) * Real(blend));
    const float lowZ = bf2x87::store(Real(low.z) * Real(blend));
    const float highX = bf2x87::store(Real(high.x) * Real(inverse));
    Real highY = Real(high.y) * Real(inverse);
    if (roundHighY)
        highY = Real(bf2x87::store(highY));
    const Vec3 result = {
        bf2x87::store(Real(highX) + Real(lowX)),
        bf2x87::store(highY + Real(lowY)),
        bf2x87::store(Real(high.z) * Real(inverse) + Real(lowZ))
    };
    return result;
#else
    (void)roundHighY;
    const Vec3 lowScaled = { low.x * blend, low.y * blend, low.z * blend };
    const Vec3 highScaled = { high.x * inverse, high.y * inverse, high.z * inverse };
    const Vec3 result = { highScaled.x + lowScaled.x,
                          highScaled.y + lowScaled.y,
                          highScaled.z + lowScaled.z };
    return result;
#endif
}
}

// PE 0x100F2740. [INFERRED] Source name and placement beside the other LightManager bodies.
// Low presets are fetched and copied before each high-preset dispatch.
void LightManager::updateExpansionLighting()
{
    LightManager* const manager = this;
    const float blend = dx9rend::rendererTailFloat(0x8BC);
    Vec3 low = manager->getSinglePointColorLow();
    const Vec3* high = &manager->getSinglePointColorHigh();
    const float inverse = 1.0f - blend;
    manager->setSinglePointColor(blendExpansionColor(low, *high, blend, inverse, false));

    low = manager->getDynamicPointColorLow();
    high = &manager->getDynamicPointColorHigh();
    manager->setDynamicPointColor(blendExpansionColor(low, *high, blend, inverse, false));

    low = manager->getSkyColorLow();
    high = &manager->getSkyColorHigh();
    manager->setSkyColor(blendExpansionColor(low, *high, blend, inverse, false));

    low = manager->getStaticSkyColorLow();
    high = &manager->getStaticSkyColorHigh();
    manager->setStaticSkyColor(blendExpansionColor(low, *high, blend, inverse, false));

    low = manager->getSunColorLow();
    high = &manager->getSunColorHigh();
    manager->setSunColor(blendExpansionColor(low, *high, blend, inverse, true));

    low = manager->getSunSpecColorLow();
    high = &manager->getSunSpecColorHigh();
    manager->setSunSpecColor(blendExpansionColor(low, *high, blend, inverse, true));

    Terrain* const terrain = static_cast<Terrain*>(g_Terrain);
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Terrain's low z product remains in x87 until the sum.
    typedef bf2x87::Real Real;
    const float lowX = bf2x87::store(Real(terrain->m_giColorLow.x) * Real(blend));
    const float lowY = bf2x87::store(Real(terrain->m_giColorLow.y) * Real(blend));
    const Real lowZ = Real(terrain->m_giColorLow.z) * Real(blend);
    const Vec3 highScaled = {
        bf2x87::store(Real(terrain->m_giColorHigh.x) * Real(inverse)),
        bf2x87::store(Real(terrain->m_giColorHigh.y) * Real(inverse)),
        bf2x87::store(Real(terrain->m_giColorHigh.z) * Real(inverse))
    };
    const Vec3 color = {
        bf2x87::store(Real(highScaled.x) + Real(lowX)),
        bf2x87::store(Real(highScaled.y) + Real(lowY)),
        bf2x87::store(Real(highScaled.z) + lowZ)
    };
#else
    const Vec3 lowScaled = { terrain->m_giColorLow.x * blend,
                            terrain->m_giColorLow.y * blend,
                            terrain->m_giColorLow.z * blend };
    const Vec3 highScaled = { terrain->m_giColorHigh.x * inverse,
                             terrain->m_giColorHigh.y * inverse,
                             terrain->m_giColorHigh.z * inverse };
    const Vec3 color = {
        highScaled.x + lowScaled.x,
        highScaled.y + lowScaled.y,
        highScaled.z + lowScaled.z
    };
#endif
    static_cast<Terrain*>(g_Terrain)->setGIColor(color.x, color.y, color.z);
}

// [CHOSEN, not recovered] Named reads of the native coordinator's flattened collector members.
// The historical accessor names are reversed: +0xB8 receives category 0 (dynamic) in
// registerWithCollector, while +0xB4 receives category 1 (static). Keep the callers' PE getter identities.
// PE 0x100EEAD0 (also 0x10190B70, the dynamic collector's frame-vector accessor, lea +0x18)
const std::vector<XpLight*>& xpStaticFrameLights() { return g_xpCoordinator.dynamicFrame; }
// PE 0x100EEAE0 (also 0x10190160, the static collector's frame-vector accessor, lea +0x1C)
const std::vector<XpLight*>& xpDynamicFrameLights() { return g_xpCoordinator.staticFrame; }

// [CHOSEN, not recovered] Named native read of the source field, PE +0x24.
world::LightSource* xpLightSource(const XpLight* light) { return light->source; }

// PE 0x1018FA30. The visual radius is explicitly stored before querying dynamic-light policy.
float xpLightRadius(const XpLight* light)
{
    if (!light->source)
        return 0.0f;
    const world::LightSourceTemplate* const sourceTemplate = lightSourceTemplatePointer(light->source);
    if (!sourceTemplate)
        return 0.0f;
    const float radius = light->source->getVisualBoundingRadius();
    if (sourceTemplate->getIsDynamic())
    {
#if defined(__APPLE__)
        return bf2x87::store(bf2x87::mul(bf2x87::wide(xpLightManager()->getDynamicLightRangeScale()),
                                       bf2x87::wide(radius)));
#else
        return xpLightManager()->getDynamicLightRangeScale() * radius;
#endif
    }
    return radius;
}

namespace {
// The six slots of the point/spot culler tables at 0x10222230/0x10222268 (cullerFalseSlot,
// pointFillRecordSlot / spotFillRecordSlot and the debug slots are the PE bodies behind slots 2, 4, 5).
class CullerSlots
{
public:
    virtual void collect(double, std::vector<world::IObject*>*, int) = 0;
    virtual bool visible(const Frustum*) = 0;
    virtual bool unused(void*) = 0;
    virtual void gather(TerrainCuller*) = 0;
    virtual void fillRecord(unsigned char*) = 0;
    virtual void debug() = 0;
};
} // anonymous namespace

// PE 0x1018FAD0. Four-slot prefix of the point/spot culler tables at 0x10222230/0x10222268.
void xpLightGatherTerrain(const XpLight* light, TerrainCuller* query)
{
    reinterpret_cast<CullerSlots*>(light->culler)->gather(query);
}

// PE 0x1018FAC0: the light's culler through slot +0x08 (a tail jump; the argument stays on the
// stack for the slot). Unreachable in 1.50: no code or data references.
bool xpLightCullerUnused(XpLight* light, void* argument)
{
    return reinterpret_cast<CullerSlots*>(light->culler)->unused(argument);
}

// PE 0x1018FAE0: the light's culler through slot +0x14 (the debug draw; a tail jump with no
// arguments). Unreachable in 1.50: no code or data references.
void xpLightCullerDebug(XpLight* light)
{
    reinterpret_cast<CullerSlots*>(light->culler)->debug();
}

// PE 0x10190E60: whether the dynamic collector's light set (collector +0x0C) holds the light
// (equal_range, then the range's length tested against zero). Unreachable in 1.50: no code or data
// references; [INFERRED] name.
bool dynamicCollectorContains(XpLight* light)
{
    return g_xpCoordinator.dynamicLights.count(light) != 0;
}

// PE 0x10190BC0 (arguments: the object, the light wrapper): calls the object's visual bounding radius
// (slot +0x90) and absolute position (slot +0x68), discarding both; an object with a geometry
// (+0x38) is tested through the light culler's slot +0x08 with its local bounds and absolute
// transformation as a BoxCorners (PE 0x100CE120) and passes when the slot answers false; an
// object without one passes. The byte at 0x1023A7A8 (initialised 1, no writer in 1.50) gates the answer.
// Unreachable in 1.50: no code or data references; [INFERRED] name.
bool lightAffectsObject(world::IObject* object, XpLight* light)
{
    (void)object->getVisualBoundingRadius();
    (void)object->getAbsolutePosition();
    bool passes = true;
    void* const geometry = candidateGeometry(object);
    if (geometry)
    {
        const LocalBoundingBox& bounds = geometryLocalBounds(geometry);
        const Mat4& transform = candidateAbsoluteTransformation(object);
        BoxCorners corners(bounds, transform);
        passes = !xpLightCullerUnused(light, &corners);
    }
    static const bool gate = true;
    return gate && passes;
}

// PE 0x101949D0: switches a light source on (slot +0x1C4 with true) and clears bit 0 of its object
// flags with a direct store, not through updateFlags. Unreachable in 1.50: no code or data
// references; [INFERRED] name.
void lightSourceEnableAndClearFlag(world::LightSource* light)
{
    light->setEnabled(true);
    world::object_detail::LayoutAccess::flags(light) &= ~1u;
}

// The 0x14-byte element of the light-cluster list PE 0x10195060 walks and PE 0x10195090 rewrites: the
// cluster's member lights (a std::vector, the first four words in the retail image) and the light
// source that stands for the cluster (+0x10). Read and written by the unreachable clustering
// helpers below; the first member is the only layout the instructions fix.
struct XpLightListEntry
{
    std::vector<world::LightSource*> lights;   // +0x00 (+0x04..+0x0C first/last/end in VC7.1)
    world::LightSource*              light;    // +0x10
};

// The 0x18-byte element PE 0x101956D0 appends: a light and its neighbour cluster entry (the entry
// starts at +0x04, its light stays null).
struct XpLightNeighbours
{
    world::LightSource* key;       // +0x00
    XpLightListEntry    entry;     // +0x04
};

// PE 0x10195060 (ecx = the list owner; its vector at +0x04..+0x08): switches every listed light
// source on and clears bit 0 of its flags, as lightSourceEnableAndClearFlag does. Unreachable in
// 1.50: no code or data references; [INFERRED] name.
void enableListedLightSources(const std::vector<XpLightListEntry>& entries)
{
    for (std::vector<XpLightListEntry>::const_iterator it = entries.begin(); it != entries.end(); ++it)
        lightSourceEnableAndClearFlag(it->light);
}

// Statistics of a light cluster (PE 0x10194CA0 result, 0x1C bytes): the mean position, the mean of
// distance + radius, the variance of that sum and its extremes.
struct XpLightClusterStats
{
    float center[3];     // +0x00
    float meanExtent;    // +0x0C
    float variance;      // +0x10
    float minExtent;     // +0x14
    float maxExtent;     // +0x18
};

// PE 0x10194CA0 (ecx = result, edx = the light list). Unreachable in 1.50: no code or data
// references; [INFERRED] name. Pass 1 averages the light positions (slot +0x80 translation), pass 2
// takes distance-to-mean + radius (slot +0x90) per light - its mean, maximum and minimum of
// distance - radius (clamped at zero) - and pass 3 the variance of distance + radius about that
// mean (the shipped call is _CIpow(x, 2)). An empty list divides by zero and answers NaN means, as
// shipped; no memory is touched. [CHOSEN, not recovered] intermediate precision: the x87 body
// keeps extended temporaries between the float stores, this form uses double.
XpLightClusterStats lightClusterStatistics(const std::vector<world::LightSource*>& lights)
{
    float sumX = 0.0f, sumY = 0.0f, sumZ = 0.0f;
    for (std::vector<world::LightSource*>::const_iterator it = lights.begin(); it != lights.end(); ++it)
    {
        const Mat4& transform = (*it)->getAbsoluteTransformation();            // slot +0x80
        sumX = static_cast<float>(static_cast<double>(sumX) + transform.m[12]);
        sumY = static_cast<float>(static_cast<double>(sumY) + transform.m[13]);
        sumZ = static_cast<float>(static_cast<double>(sumZ) + transform.m[14]);
    }
    const double count = static_cast<double>(static_cast<unsigned int>(lights.size()));
    const double inverse = 1.0 / count;
    const float cx = static_cast<float>(sumX * inverse);
    const float cy = static_cast<float>(sumY * inverse);
    const float cz = static_cast<float>(sumZ * inverse);

    float maxExtent = 0.0f;
    float sumExtent = 0.0f;
    float minExtent = FLT_MAX;
    for (std::vector<world::LightSource*>::const_iterator it = lights.begin(); it != lights.end(); ++it)
    {
        const Mat4& transform = (*it)->getAbsoluteTransformation();
        const double dx = static_cast<double>(cx) - transform.m[12];
        const double dy = static_cast<double>(cy) - transform.m[13];
        const double dz = static_cast<double>(cz) - transform.m[14];
        const float distance = static_cast<float>(std::sqrt((dz * dz + dy * dy) + dx * dx));
        const double radius = (*it)->getVisualBoundingRadius();                // slot +0x90
        const double outer = static_cast<double>(distance) + radius;
        sumExtent = static_cast<float>(static_cast<double>(sumExtent) + outer);
        const float outerFloat = static_cast<float>(outer);
        if (maxExtent < outerFloat)
            maxExtent = outerFloat;
        const double inner = static_cast<double>(distance) - radius;
        if (inner < static_cast<double>(minExtent))
            minExtent = static_cast<float>(inner);
    }
    const float meanExtent = static_cast<float>(sumExtent / count);
    if (minExtent < 0.0f)
        minExtent = 0.0f;

    float varianceSum = 0.0f;
    for (std::vector<world::LightSource*>::const_iterator it = lights.begin(); it != lights.end(); ++it)
    {
        const Mat4& transform = (*it)->getAbsoluteTransformation();
        const float dx = cx - transform.m[12];
        const float dy = cy - transform.m[13];
        const float dz = cz - transform.m[14];
        const double radius = (*it)->getVisualBoundingRadius();
        const double distance = std::sqrt((static_cast<double>(dz) * dz + static_cast<double>(dy) * dy)
                                          + static_cast<double>(dx) * dx);
        const double deviation = (distance + radius) - meanExtent;
        varianceSum = static_cast<float>(static_cast<double>(varianceSum) + std::pow(deviation, 2.0));
    }

    XpLightClusterStats result;
    result.center[0] = cx;
    result.center[1] = cy;
    result.center[2] = cz;
    result.meanExtent = meanExtent;
    result.variance = static_cast<float>(varianceSum / count);
    result.minExtent = minExtent;
    result.maxExtent = maxExtent;
    return result;
}

// PE 0x10194F10 (ecx = the light list, edx = the cluster light, spread and boost on the stack,
// retn 8). Unreachable in 1.50: no code or data references; [INFERRED] name. Moves the cluster
// light to the cluster's mean position with an identity rotation, then retunes its template:
// attenuation range 1 becomes the mean extent, range 2 (1 + boost) times a radius that lies between
// mean + standard deviation and the largest extent (spread > 0 interpolates towards the largest
// extent, otherwise the deviation is scaled by 1 + spread), and the colour and specular colour are
// set to white. The template is shared; the answer is the light. The template slots are +0x14C /
// +0x154 / +0x15C / +0x164 of the light template (PE; the ELF table sits one slot later).
world::LightSource* fitLightToCluster(const std::vector<world::LightSource*>& lights,
                                      world::LightSource* light, float spread, float boost)
{
    const XpLightClusterStats stats = lightClusterStatistics(lights);
    const double deviation = std::sqrt(static_cast<double>(stats.variance));
    double radius;
    if (spread > 0.0f)
    {
        const double base = deviation + stats.meanExtent;
        radius = base + (static_cast<double>(stats.maxExtent) - base) * spread;
    }
    else
        radius = ((deviation + stats.meanExtent) - stats.meanExtent) * (spread + 1.0f) + stats.meanExtent;
    const float scaledRadius = static_cast<float>((boost + 1.0f) * radius);

    Mat4 transform;
    for (int i = 0; i < 16; ++i)
        transform.m[i] = 0.0f;
    transform.m[0] = 1.0f;
    transform.m[5] = 1.0f;
    transform.m[10] = 1.0f;
    transform.m[12] = stats.center[0];
    transform.m[13] = stats.center[1];
    transform.m[14] = stats.center[2];
    transform.m[15] = 1.0f;
    world::LightSourceTemplate* const objectTemplate =
        const_cast<world::LightSourceTemplate*>(lightSourceTemplatePointer(light));
    light->setAbsoluteTransformation(transform);                               // slot +0x88
    objectTemplate->setAttenuationRange1(stats.meanExtent);                    // template +0x14C
    objectTemplate->setAttenuationRange2(scaledRadius);                        // template +0x154
    Vec3 white = { 1.0f, 1.0f, 1.0f };
    objectTemplate->setColor(white);                                           // template +0x15C
    white.x = 1.0f; white.y = 1.0f; white.z = 1.0f;
    objectTemplate->setSpecColor(white);                                       // template +0x164
    return light;
}

world::LightSource* xpCreateOwnedLight(unsigned int argument);   // PE 0x100EF380, defined below

// PE 0x10195090 (ecx = the cluster list, spread and boost on the stack, retn 8). Unreachable in
// 1.50: no code or data references; [INFERRED] name. For every cluster: its light becomes the first
// member, then a fresh owned point light (PE 0x100EF380, xpCreateOwnedLight), then that light fitted to the members
// (fitLightToCluster). The smallest and largest member counts are computed and discarded.
// [CHOSEN] an empty member list leaves the light untouched in the first step: the retail body reads
// the first element through a null pointer, which the native backends fault on.
void fitListedLightClusters(std::vector<XpLightListEntry>& entries, float spread, float boost)
{
    float smallest = FLT_MAX;
    float largest = 0.0f;
    for (std::vector<XpLightListEntry>::iterator it = entries.begin(); it != entries.end(); ++it)
    {
        const float count = static_cast<float>(static_cast<unsigned int>(it->lights.size()));
        smallest = (smallest < count) ? smallest : count;
        largest = (count < largest) ? largest : count;
        if (!it->lights.empty())
            it->light = it->lights.front();
        it->light = xpCreateOwnedLight(1);
        it->light = fitLightToCluster(it->lights, it->light, spread, boost);
    }
    (void)smallest;
    (void)largest;
}

void fitLightToSize(world::LightSource* light, const Vec3* direction, float size);   // defined below (PE 0x10194B50)

// PE 0x10195140 (ecx = the light list, edx = the pass count). Unreachable in 1.50: no code or data
// references; [INFERRED] name. Up to `passes` times, every ordered pair of distinct lights whose
// spheres overlap (distance - radii < 0) pushes the second light away from the first with
// fitLightToSize (PE 0x10194B50): direction = -normalize(second - first), size = -gap * 0.25. The
// passes stop when one pass moved nothing. The x87 body keeps the dy and dz differences and the
// gap in extended precision; this form uses double for them.
void separateOverlappingLights(const std::vector<world::LightSource*>& lights, int passes)
{
    for (int pass = 0; pass < passes; ++pass)
    {
        bool moved = false;
        for (std::vector<world::LightSource*>::const_iterator outer = lights.begin();
             outer != lights.end(); ++outer)
        {
            for (std::vector<world::LightSource*>::const_iterator inner = lights.begin();
                 inner != lights.end(); ++inner)
            {
                if (*outer == *inner)
                    continue;
                const Mat4& innerTransform = (*inner)->getAbsoluteTransformation();    // slot +0x80
                const float innerRadius = (*inner)->getVisualBoundingRadius();         // slot +0x90
                const Mat4& outerTransform = (*outer)->getAbsoluteTransformation();
                const float outerRadius = (*outer)->getVisualBoundingRadius();
                const float dx = outerTransform.m[12] - innerTransform.m[12];
                const double dy = static_cast<double>(outerTransform.m[13]) - innerTransform.m[13];
                const double dz = static_cast<double>(outerTransform.m[14]) - innerTransform.m[14];
                const double distance = std::sqrt((dz * dz + dy * dy) + static_cast<double>(dx) * dx);
                const double gapExact = distance - (static_cast<double>(outerRadius) + innerRadius);
                const float gap = static_cast<float>(gapExact);
                if (!(gapExact < 0.0))
                    continue;
                Vec3 direction;
                {
                    const Mat4& firstTransform = (*outer)->getAbsoluteTransformation();
                    (void)(*outer)->getVisualBoundingRadius();
                    const Mat4& secondTransform = (*inner)->getAbsoluteTransformation();
                    (void)(*inner)->getVisualBoundingRadius();
                    direction.x = secondTransform.m[12] - firstTransform.m[12];
                    direction.y = secondTransform.m[13] - firstTransform.m[13];
                    direction.z = secondTransform.m[14] - firstTransform.m[14];
                }
                shadowNormalizeInPlace(direction);
                Vec3 away;
                away.x = -direction.x;
                away.y = -direction.y;
                away.z = -direction.z;
                fitLightToSize(*inner, &away, gap * -1.0f * 0.25f);
                moved = true;
            }
        }
        if (!moved)
            break;
    }
}

// PE 0x10195330 (ecx = the cluster list, edx = the pool of lights, the scale on the stack, retn 4;
// answers al). Unreachable in 1.50: no code or data references; [INFERRED] name. For every cluster,
// each pooled light whose sphere overlaps the cluster light's - (r1 + r2)^2 * scale^2 against the
// squared distance, summed z, y, x - joins the cluster's members and leaves the pool (the tail is
// moved down). Answers whether any light moved.
bool absorbOverlappingLights(std::vector<XpLightListEntry>& entries,
                             std::vector<world::LightSource*>& pool, float scale)
{
    bool absorbed = false;
    for (std::vector<XpLightListEntry>::iterator entry = entries.begin(); entry != entries.end(); ++entry)
    {
        std::vector<world::LightSource*>::size_type index = 0;
        while (index != pool.size())
        {
            world::LightSource* const candidate = pool[index];
            const Mat4& candidateTransform = candidate->getAbsoluteTransformation();   // slot +0x80
            const float candidateRadius = candidate->getVisualBoundingRadius();        // slot +0x90
            const Mat4& clusterTransform = entry->light->getAbsoluteTransformation();
            const double clusterRadius = entry->light->getVisualBoundingRadius();
            const float dx = clusterTransform.m[12] - candidateTransform.m[12];
            const double dy = static_cast<double>(clusterTransform.m[13]) - candidateTransform.m[13];
            const double dz = static_cast<double>(clusterTransform.m[14]) - candidateTransform.m[14];
            const double radii = clusterRadius + candidateRadius;
            const double squaredDistance = (dz * dz + dy * dy) + static_cast<double>(dx) * dx;
            if (radii * radii * scale * scale > squaredDistance)
            {
                entry->lights.push_back(candidate);
                pool.erase(pool.begin() + index);
                absorbed = true;
            }
            else
                ++index;
        }
    }
    return absorbed;
}

// PE 0x101955C0 (ecx = the light list, edx = the keep set, the remainder set on the stack, retn 4).
// Unreachable in 1.50: no code or data references; [INFERRED] name. Keeps only the lights that are
// in the keep set (an empty keep set also releases the list's storage), then adds keep \ list to the
// remainder set. The shipped body copies the kept lights into a scratch set first (hinted inserts),
// removes the others with find_if + remove_copy_if and finishes with set_difference into an insert
// iterator at the remainder's begin.
void intersectLightList(std::vector<world::LightSource*>& list,
                        const std::set<world::LightSource*>& keep,
                        std::set<world::LightSource*>& remainder)
{
    if (keep.empty())
    {
        std::vector<world::LightSource*>().swap(list);
        return;
    }
    std::set<world::LightSource*> kept;
    for (std::vector<world::LightSource*>::const_iterator it = list.begin(); it != list.end(); ++it)
    {
        if (keep.find(*it) != keep.end())
            kept.insert(kept.begin(), *it);
    }
    std::vector<world::LightSource*>::iterator write = list.begin();
    while (write != list.end() && keep.find(*write) != keep.end())
        ++write;
    if (write != list.end())
    {
        for (std::vector<world::LightSource*>::iterator read = write + 1; read != list.end(); ++read)
        {
            if (keep.find(*read) != keep.end())
                *write++ = *read;
        }
        list.erase(write, list.end());
    }
    std::set_difference(keep.begin(), keep.end(), kept.begin(), kept.end(),
                        std::inserter(remainder, remainder.begin()));
}

// PE 0x101956D0 (ecx = the light list, edx = the output, the scale on the stack, retn 4).
// Unreachable in 1.50: no code or data references; [INFERRED] name. Appends, for every light, an
// element holding the lights of the list (other than itself) whose spheres overlap its own - the
// overlap test of absorbOverlappingLights, summed z, y, x.
void collectNeighbourLists(const std::vector<world::LightSource*>& lights,
                           std::vector<XpLightNeighbours>& output, float scale)
{
    for (std::vector<world::LightSource*>::const_iterator outer = lights.begin(); outer != lights.end(); ++outer)
    {
        XpLightNeighbours element;
        element.key = *outer;
        element.entry.light = 0;
        for (std::vector<world::LightSource*>::const_iterator inner = lights.begin();
             inner != lights.end(); ++inner)
        {
            if (*outer == *inner)
                continue;
            const Mat4& innerTransform = (*inner)->getAbsoluteTransformation();        // slot +0x80
            const float innerRadius = (*inner)->getVisualBoundingRadius();             // slot +0x90
            const Mat4& outerTransform = element.key->getAbsoluteTransformation();
            const double outerRadius = element.key->getVisualBoundingRadius();
            const float dx = outerTransform.m[12] - innerTransform.m[12];
            const double dy = static_cast<double>(outerTransform.m[13]) - innerTransform.m[13];
            const double dz = static_cast<double>(outerTransform.m[14]) - innerTransform.m[14];
            const double radii = outerRadius + innerRadius;
            const double squaredDistance = (dz * dz + dy * dy) + static_cast<double>(dx) * dx;
            if (radii * radii * scale * scale > squaredDistance)
                element.entry.lights.push_back(*inner);
        }
        output.push_back(element);
    }
}

// The order of the neighbour lists in groupLightsByNeighbours, the longer member list first
// (instructions at 0x10194B10, a fragment that Ghidra folds into the preceding function). Unreachable in 1.50: no code or data
// references; [INFERRED] name.
bool neighbourListLonger(const XpLightNeighbours& first, const XpLightNeighbours& second)
{
    return first.entry.lights.size() > second.entry.lights.size();
}

// PE 0x10195840 (ecx = the light list, edx = the neighbour lists, the clusters and the loose lights
// on the stack, retn 8). Unreachable in 1.50: no code or data references; [INFERRED] name. Sorts the
// neighbour lists (longest first, PE 0x10194B10) and walks them with a set of the still unassigned
// lights: each list gains its own light, is cut down to the unassigned lights (intersectLightList,
// which also yields the unassigned lights left over) and, when more than one remains, is appended to
// the clusters (PE 0x10197DD0); otherwise its light is appended to the loose lights. The unassigned
// set becomes the leftover set after every element. The sort is std::sort with the retail
// predicate; elements of equal length may end up in a different order than the shipped
// introsort leaves them in.
void groupLightsByNeighbours(const std::vector<world::LightSource*>& lights,
                             std::vector<XpLightNeighbours>& neighbours,
                             std::vector<XpLightListEntry>& clusters,
                             std::vector<world::LightSource*>& loose)
{
    std::sort(neighbours.begin(), neighbours.end(), neighbourListLonger);
    std::set<world::LightSource*> unassigned(lights.begin(), lights.end());
    for (std::vector<XpLightNeighbours>::iterator it = neighbours.begin(); it != neighbours.end(); ++it)
    {
        it->entry.lights.push_back(it->key);
        std::set<world::LightSource*> remainder;
        intersectLightList(it->entry.lights, unassigned, remainder);
        unassigned.clear();
        unassigned.swap(remainder);
        if (it->entry.lights.size() > 1)
            clusters.push_back(it->entry);
        else
            loose.push_back(it->key);
    }
    unassigned.clear();
}

// PE 0x10190C40 (ecx = the map owner, whose tree head is at +0x04): the in-order walk of a std::map
// with pointer-sized elements, answering the key (node +0x0C) of the last entry whose mapped value
// (node +0x10) equals the argument, or null. The walk does not stop at the first match. [INFERRED]
// owner: the coordinator's source-to-wrapper map is the only such map; unreachable in 1.50: no code
// or data references.
world::LightSource* sourceOfXpLight(const XpLight* wanted)
{
    world::LightSource* result = 0;
    for (std::map<world::LightSource*, XpLight*>::const_iterator it = g_xpCoordinator.bySource.begin();
         it != g_xpCoordinator.bySource.end(); ++it)
    {
        if (it->second == wanted)
            result = it->first;
    }
    return result;
}

// PE 0x10190CB0: whether sourceOfXpLight finds a source for the wrapper. Unreachable in 1.50: no
// code or data references; [INFERRED] name.
bool isXpLightRegistered(const XpLight* wanted)
{
    return sourceOfXpLight(wanted) != 0;
}

// PE 0x10194AD0 (ecx = result, edx = light): a light's bounding sphere - the translation of its
// absolute transformation and its visual bounding radius. Unreachable in 1.50: no code or data
// references; [INFERRED] name.
void lightSourceBoundingSphere(float sphere[4], world::LightSource* light)
{
    const Mat4& transform = light->getAbsoluteTransformation();            // slot +0x80
    const float radius = light->getVisualBoundingRadius();                 // slot +0x90
    sphere[0] = transform.m[12];
    sphere[1] = transform.m[13];
    sphere[2] = transform.m[14];
    sphere[3] = radius;
}

// PE 0x10194A70 (ecx = first sphere, eax = second sphere, scale on the stack): whether
// (r1 + r2)^2 * scale^2 exceeds the squared centre distance, summed as (dx^2 + dz^2) + dy^2. The
// shipped x87 body keeps extended intermediates; this form rounds each step to float. Unreachable in
// 1.50: no code or data references; [INFERRED] name.
bool spheresOverlapScaled(const float first[4], const float second[4], float scale)
{
    const float dx = first[0] - second[0];
    const float dy = first[1] - second[1];
    const float dz = first[2] - second[2];
    const float radii = first[3] + second[3];
    const float distanceSquared = (dx * dx + dz * dz) + dy * dy;
    return radii * radii * scale * scale > distanceSquared;
}

// PE 0x10194B50 (ecx = light, edx = direction, size on the stack). Unreachable in 1.50: no code or
// data references; [INFERRED] name. Fits a light around an object: the size is replaced by a quarter
// of the light's radius when that radius exceeds twice the size; the light moves back along the
// direction by 0.1 * size * direction; its second attenuation range becomes radius - 0.1 * size and
// its first half of that range.
void fitLightToSize(world::LightSource* light, const Vec3* direction, float size)
{
    (void)light->getAbsoluteTransformation();                              // slot +0x80, discarded
    if (light->getVisualBoundingRadius() > size + size)                    // slot +0x90
        size = light->getVisualBoundingRadius() * 0.25f;
    Mat4 transform(light->getAbsoluteTransformation());
    const float x = size * direction->x * 0.1f;
    const float y = size * direction->y * 0.1f;
    const float z = size * direction->z * 0.1f;
    transform.m[12] -= x;
    transform.m[13] -= y;
    transform.m[14] -= z;
    light->setAbsoluteTransformation(transform);                           // slot +0x88
    (void)light->getAbsoluteTransformation();
    light->setAttenuationRange2(light->getVisualBoundingRadius() - size * 0.1f);   // slot +0x18C
    light->setAttenuationRange1(light->getAttenuationRange2() * 0.5f);             // slots +0x190, +0x184
}

// PE 0x10190B80: the light wrapper for a source - null for a null source or a failed 0x38-byte
// allocation (PE 0x1005F320), else the constructed wrapper (PE 0x1018FF40). Unreachable in 1.50: no
// code or data references.
XpLight* createXpLight(world::LightSource* source)
{
    if (!source)
        return 0;
    XpLight* const light = static_cast<XpLight*>(::renddx9abi::engineAlloc(sizeof(XpLight)));
    if (!light)
        return 0;
    constructXpLight(light, source);
    return light;
}

// PE 0x10190150: stores the collector's flags dword (+0x00; bit 0x1 and 0x10 select the debug
// draws of the gather and budget steps). Unreachable in 1.50: no code or data references, so
// the flags stay at their constructed zero.
void setXpCollectorFlags(unsigned int* flags, unsigned int value)
{
    *flags = value;
}

XpLight* xpLightForSource(world::LightSource* source)
{
    // PE 0x100EF3F0: count first, then operator[] on the coordinator's source hash map.
    if (g_xpCoordinator.bySource.count(source) != 0)
        return g_xpCoordinator.bySource[source];
    return 0;
}

void appendXpLightsForGeometry(void* geometry, std::vector<XpLight*>& output)
{
    // PE 0x100EF4E0: append the +0xA4 multimap's equal-key range without clearing output.
    typedef std::multimap<void*, world::LightSource*> Associations;
    const Associations& pairs = g_xpCoordinator.geometryPairs;
    Associations::const_iterator it = pairs.lower_bound(geometry);
    while (it != pairs.end() && it->first == geometry)
    {
        // The shipped query appends even when the source lookup returns null.
        output.push_back(xpLightForSource(it->second));
        ++it;
    }
}

// Unreachable island of the coordinator (root PE 0x10195090 has no code or data reference). The
// bodies below are recovered from the listings and invent no callers. `self` of the retail methods
// is the coordinator, so the hash tables at +0x00 / +0x48 / +0x70 map onto the members of
// g_xpCoordinator (see its declaration for the [CHOSEN, not recovered] types).

// PE 0x100EEAF0: the unused stack argument is cleaned by `retn 4`, never read, and its type is
// [CHOSEN, not recovered]. unreachable in 1.50: no code or data references
// Creates "XpLight%p" from the "LightSource" class, as a never-dynamic enabled Point light, and
// returns its object. [PLATFORM] %p formatting differs between the CRTs; the name is only a key.
world::LightSource* xpCreatePointLightObject(const void* self, unsigned int)
{
    world::ObjectTemplateManager* const templateManager =
        static_cast<world::ObjectTemplateManager*>(classManager->getSingleton(
            renddx9abi::HostString("ObjectTemplateManager")));

    char buffer[0x134];
    std::snprintf(buffer, sizeof(buffer), "%p", self);
    renddx9abi::HostString objectName("XpLight");
    objectName += renddx9abi::HostString(buffer);

    world::LightSourceTemplate* objectTemplate;
    {
        const renddx9abi::HostString className("LightSource");
        objectTemplate = static_cast<world::LightSourceTemplate*>(
            templateManager->createTemplate(hostStdString(className), hostStdString(objectName)));
    }
    objectTemplate->addRef();
    objectTemplate->updateFlags(0u, 0u);
    objectTemplate->setEnabled(true);
    objectTemplate->setIsDynamic(false);
    {
        const renddx9abi::HostString type("Point");
        objectTemplate->setLightType(hostStdString(type));
    }
    world::LightSource* const light =
        static_cast<world::LightSource*>(objectTemplate->createObject());
    light->addRef();
    return light;
}

// PE 0x100EECC0: unreachable in 1.50: no code or data references
// Destroys a wrapper (PE 0x1018FFB0) and frees its storage; null is ignored. `this` is not read.
void xpDestroyAndFree(XpLight* light)
{
    if (light)
    {
        destroyXpLight(light);
        ::renddx9abi::engineFree(light);
    }
}

// PE 0x100EECE0: unreachable in 1.50: no code or data references
// Whether a wrapper is a mapped value of the registration table (walks the hash list's nodes).
bool xpWrapperIsRegistered(const XpLight* light)
{
    for (std::map<world::LightSource*, XpLight*>::const_iterator it =
             g_xpCoordinator.bySource.begin(); it != g_xpCoordinator.bySource.end(); ++it)
    {
        if (it->second == light)
            return true;
    }
    return false;
}

// PE 0x100EEDA0: unreachable in 1.50: no code or data references
// Whether the +0x70 lookup holds the key (hash equal_range count != 0).
bool xpRecordLookupCContains(const void* key)
{
    return g_xpCoordinator.recordLookupC.count(key) != 0;
}

// PE 0x100EEDE0: unreachable in 1.50: no code or data references
// Whether the +0x48 lookup holds the key (hash equal_range count != 0).
bool xpRecordLookupBContains(const void* key)
{
    return g_xpCoordinator.recordLookupB.count(key) != 0;
}

// PE 0x100EF380: the unused stack argument is forwarded to 0x100EEAF0 and its type is
// [CHOSEN, not recovered]. unreachable in 1.50: no code or data references
// Creates a Point light object, records it as owned, wraps and registers it with the creation flag
// raised around the registration, and returns the object. A failed wrapper allocation is passed on
// to the collector unchecked, as shipped.
world::LightSource* xpCreateOwnedLight(unsigned int argument)
{
    world::LightSource* const light = xpCreatePointLightObject(&g_xpCoordinator, argument);
    g_xpCoordinator.ownedLights.insert(light);
    g_xpCoordinator.registeringOwnedLight = 1;
    XpLight* wrapper = 0;
    void* const storage = ::renddx9abi::engineAlloc(sizeof(XpLight));
    if (storage)
    {
        constructXpLight(static_cast<XpLight*>(storage), light);
        wrapper = static_cast<XpLight*>(storage);
    }
    // PE 0x100EF050 also publishes (light, wrapper) in the registration table.
    registerWithCollector(wrapper);
    g_xpCoordinator.bySource.insert(std::make_pair(light, wrapper));
    g_xpCoordinator.iterationOrder.push_back(wrapper);
    g_xpCoordinator.registeringOwnedLight = 0;
    return light;
}

// PE 0x100EF480: unreachable in 1.50: no code or data references
// Fills a light record from the registered wrapper of `source`; nothing happens when unregistered.
void xpFillRecordForSource(const Mat4& inverse, world::LightSource* source,
                           dx9rend::Ra::RaLightRecord& output)
{
    if (g_xpCoordinator.bySource.count(source) != 0)
        xpLightFillRecord(xpLightForSource(source), inverse, output);
}

// PE 0x100EF5E0: unreachable in 1.50: no code or data references
// Marks the registered wrapper of `source` active; nothing happens when unregistered.
void xpActivateSource(world::LightSource* source)
{
    if (g_xpCoordinator.bySource.count(source) != 0)
        setXpLightActive(g_xpCoordinator.bySource[source], true);
}

// PE 0x100EF670: unreachable in 1.50: no code or data references
// Marks the registered wrapper of `source` inactive; nothing happens when unregistered.
void xpDeactivateSource(world::LightSource* source)
{
    if (g_xpCoordinator.bySource.count(source) != 0)
        setXpLightActive(g_xpCoordinator.bySource[source], false);
}

// PE 0x100EF7F0: the record mapped to `key` in the +0x48 lookup (find-or-insert, then the value).
// Reached only from the unreachable PE 0x100EF960.
LightMergerRecord* xpLookupRecordB(const void* key)
{
    return g_xpCoordinator.recordLookupB[key];
}

// PE 0x100EF7A0: the record mapped to `key` in the +0x70 lookup (find-or-insert, then the value).
// Reached only from the unreachable PE 0x100EF9F0.
LightMergerRecord* xpLookupRecordC(const void* key)
{
    return g_xpCoordinator.recordLookupC[key];
}

// [CHOSEN, not recovered] Unobservable-type exception: the element type of the vector PE
// 0x100EF700 walks. Each element is the LightMerger key itself, and its dword at +0x38 is the
// +0x48 lookup key; nothing calls 0x100EF700, so no element type can be observed.
struct XpMergedRecordOwner
{
    unsigned char unread[0x38];
    const void* recordKey;                              // +0x38
};

// PE 0x100EF700: unreachable in 1.50: no code or data references
// For every element the merger has a record for, stores that record in the +0x48 lookup under
// the element's +0x38 key (find-or-insert, then assign). The end is re-read every iteration.
void xpCacheMergedRecords(const std::vector<XpMergedRecordOwner*>& owners)
{
    for (std::vector<XpMergedRecordOwner*>::const_iterator it = owners.begin(); it != owners.end();
         ++it)
    {
        XpMergedRecordOwner* const owner = *it;
        LightMergerRecord* const record = g_xpCoordinator.merger.findRecord(owner);
        if (record)
            g_xpCoordinator.recordLookupB[owner->recordKey] = record;
    }
}

// PE 0x100EF960: unreachable in 1.50: no code or data references
// When the +0x48 lookup holds `from`, stores its mapped record (PE 0x100EF7F0) under `to` in the
// +0x70 lookup (find-or-insert, then assign).
void xpAliasRecord(const void* from, const void* to)
{
    if (g_xpCoordinator.recordLookupB.count(from) != 0)
    {
        LightMergerRecord* const record = xpLookupRecordB(from);
        g_xpCoordinator.recordLookupC[to] = record;
    }
}

// PE 0x100EF9F0: unreachable in 1.50: no code or data references
// When the +0x70 lookup holds `key`: the first time, saves the light manager's sun and sky colours
// into the saved record; then publishes the mapped record (PE 0x100EF7A0) and answers true.
// Otherwise a saved record is published back and forgotten (true); with nothing saved, false.
bool xpApplyMergedRecord(const void* key)
{
    if (g_xpCoordinator.recordLookupC.count(key) != 0)
    {
        if (!g_xpCoordinator.sunSaved)
        {
            g_xpCoordinator.savedSunRecord.captureLightManagerColors();
            g_xpCoordinator.sunSaved = true;
        }
        xpLookupRecordC(key)->applyToLightManager();
        return true;
    }
    if (g_xpCoordinator.sunSaved)
    {
        g_xpCoordinator.savedSunRecord.applyToLightManager();
        g_xpCoordinator.sunSaved = false;
        return true;
    }
    return false;
}

// PE 0x100EFAA0: unreachable in 1.50: no code or data references
// Empties the geometry association multimap (+0xA4) and both record lookups (+0x48, +0x70).
void xpClearAssociationTables()
{
    g_xpCoordinator.geometryPairs.clear();
    g_xpCoordinator.recordLookupB.clear();
    g_xpCoordinator.recordLookupC.clear();
}

// PE 0x100F22D0: unreachable in 1.50: no code or data references
// [INFERRED] source name and placement beside the other simple-shadow bodies. Stores the two-bit
// cell of `value` (scaled by 3/255, rounded) at column x of row z of the packed map: byte
// row z * stride + (x >> 2) with the arithmetic shift, low cell first. Unlike the sampler
// (PE 0x100F1F10) it applies no origin offsets. loadSimpleShadowMaps carries the same arithmetic
// inline for each pixel of the raw file.
void simpleShadowMapStoreTexel(void* self, int x, int z, unsigned char value)
{
    const unsigned int cell = static_cast<unsigned int>(std::floor(
        static_cast<double>(value) * static_cast<double>(3.0f / 255.0f) + 0.5));
    const unsigned int shift = static_cast<unsigned int>((x & 3) * 2);
    unsigned char& destination = lm(self).simpleShadowMap[
        lm(self).simpleShadowStride * z + (x >> 2)];
    destination = static_cast<unsigned char>((destination & ~(3u << shift)) | ((cell & 3u) << shift));
}

// PE 0x100EF840: destroys every registered wrapper, empties the registration table, then deletes
// every owned light object (virtual slot +0x14 with flag 1, the deleting destructor) and empties
// the owned set. Retail reaches it from the coordinator destructor (PE 0x100EFBA0, at exit) and
// from the unreferenced PE 0x100EFB90. The insertion-order mirror of the registration table is
// native-only and is emptied with it.
void xpClearCoordinator()
{
    for (std::map<world::LightSource*, XpLight*>::iterator it = g_xpCoordinator.bySource.begin();
         it != g_xpCoordinator.bySource.end(); ++it)
    {
        XpLight* const light = it->second;
        unregisterFromCollector(light);
        if (light)
        {
            destroyXpLight(light);
            ::renddx9abi::engineFree(light);
        }
    }
    g_xpCoordinator.bySource.clear();
    g_xpCoordinator.iterationOrder.clear();
    for (std::set<world::LightSource*>::iterator it = g_xpCoordinator.ownedLights.begin();
         it != g_xpCoordinator.ownedLights.end(); ++it)
    {
        if (*it)
            delete *it;
    }
    g_xpCoordinator.ownedLights.clear();
}

int xpLightCategory(const XpLight* light)
{
    // PE 0x1018FB90. Named native-layout access to the retail +0x30 field.
    return light->category;
}

int xpLightType(const XpLight* light)
{
    // PE 0x1018FBA0.
    return lightSourceTemplatePointer(light->source)->getLightTypeEnum();
}

void xpLightFillRecord(const XpLight* light, const Mat4& inverse,
                       dx9rend::Ra::RaLightRecord& output)
{
    // PE 0x1018FBC0. [INFERRED] placement beside the existing XpLight.cpp bodies.
    const Mat4& transform = light->source->getAbsoluteTransformation();
    const Vec3 direction = { transform.m[8], transform.m[9], transform.m[10] };
#if defined(__APPLE__)
    // [CHOSEN, not recovered] Retain x87 intermediates until the three stores.
    typedef bf2x87::Real Real;
#else
    typedef float Real;
#endif
    const float* const m = inverse.m;
    output.direction[0] = static_cast<float>((Real(m[8]) * Real(direction.z)
        + Real(m[4]) * Real(direction.y)) + Real(direction.x) * Real(m[0]));
    output.direction[1] = static_cast<float>((Real(m[9]) * Real(direction.z)
        + Real(m[5]) * Real(direction.y)) + Real(m[1]) * Real(direction.x));
    output.direction[2] = static_cast<float>((Real(m[10]) * Real(direction.z)
        + Real(m[6]) * Real(direction.y)) + Real(m[2]) * Real(direction.x));

    // PE 0x100777A0's normalization, including the unit-length and degenerate early outs.
    // [INFERRED] local spelling: the other recovered copy is TU-local in BundledMeshDraw.cpp.
    const float lengthSquared = static_cast<float>(
        (Real(output.direction[0]) * Real(output.direction[0])
         + Real(output.direction[1]) * Real(output.direction[1]))
        + Real(output.direction[2]) * Real(output.direction[2]));
    if (!(std::fabs(static_cast<double>(Real(lengthSquared) - Real(1.0f))) < FLT_EPSILON))
    {
        if (std::fabs(lengthSquared) < FLT_EPSILON)
            output.direction[0] = output.direction[1] = output.direction[2] = 0.0f;
        else
        {
#if defined(__APPLE__)
            const Real scale = Real(1.0f) / bf2x87::squareRoot(Real(lengthSquared));
#else
            const Real scale = 1.0f / std::sqrt(lengthSquared);
#endif
            for (unsigned int i = 0; i < 3; ++i)
                output.direction[i] = static_cast<float>(scale * Real(output.direction[i]));
        }
    }

    float rangeScale = 1.0f;
    if (light->category == 0)
        rangeScale = g_LightManager->getDynamicLightRangeScale();
    const Mat4& positioned = light->source->getAbsoluteTransformation();
    const Vec3 position = { positioned.m[12], positioned.m[13], positioned.m[14] };
    output.position[0] = static_cast<float>(((Real(m[8]) * Real(position.z)
        + Real(m[4]) * Real(position.y)) + Real(position.x) * Real(m[0])) + Real(m[12]));
    output.position[1] = static_cast<float>(((Real(m[1]) * Real(position.x)
        + Real(m[9]) * Real(position.z)) + Real(m[5]) * Real(position.y)) + Real(m[13]));
    output.position[2] = static_cast<float>(((Real(m[2]) * Real(position.x)
        + Real(m[10]) * Real(position.z)) + Real(m[6]) * Real(position.y)) + Real(m[14]));
    const Real range = Real(light->source->getAttenuationRange2()) * Real(rangeScale);
    output.invRangeSq = static_cast<float>(Real(1.0f) / (range * range));
    const Vec3& color = light->source->getColor();
    output.color[0] = color.x;
    output.color[1] = color.y;
    output.color[2] = color.z;
    const Vec3& specular = light->source->getSpecColor();
    output.unread28[0] = specular.x;
    output.unread28[1] = specular.y;
    output.unread28[2] = specular.z;
    typedef void (__thiscall* FillFn)(void*, unsigned char*);
    reinterpret_cast<FillFn>(const_cast<void*>(light->culler->vtable[0x10 / 4]))(
        light->culler, reinterpret_cast<unsigned char*>(&output));
    // ds:102637DC has no writer in the image (only the CRT teardown also references it).
    // The optional +0x14 debug dispatch is consequently unreachable in the shipped image.
}

void submitSourceLightRecord(world::LightSource* source, const Mat4& inverse,
                             dx9rend::RenderQueueEntry* entry, bool transparent)
{
    // PE 0x100FB8F0 constructs/destroys this transient wrapper with 0x1018FF40/0x1018FFB0.
    alignas(XpLight) unsigned char storage[sizeof(XpLight)];
    XpLight* const light = reinterpret_cast<XpLight*>(storage);
    constructXpLight(light, source);
    xpLightFillRecord(light, inverse,
        *static_cast<dx9rend::Ra::RaLightRecord*>(entry->lightArray.lights.value));
    dx9rend::renderQueueEnqueue(entry, transparent, false, true);
    destroyXpLight(light);
}

void initializeLightManager(LightManager* manager)
{
    initializeLightManagerBody(manager);
}

void LightManager::loadSimpleShadowMaps()
{
    loadSimpleShadowMapsBody(this);
}

// PE 0x100EFAF0. Retail also clears the two source lookup hash tables (+0x48/+0x70); their only
// inserters (PE 0x100EF7A0 / 0x100EF7F0) are reached from unreferenced roots, so they are always
// empty and this reset leaves the native lookups (recordLookupB / recordLookupC) alone.
void resetXpTransientState()
{
    g_xpCoordinator.geometryPairs.clear();
    g_xpCoordinator.merger.clearTransient();
    LightMerger::releaseRecords();
}

// PE 0x100EF100. The containers start empty, the merger, the collectors and the saved record are
// built (retail: heap-allocated, see the struct), and both flag bytes (+0xBC, +0xC4) start zero.
XpCoordinator::XpCoordinator()
    : sunSaved(false), staticFlags(0), staticTime(0.0), dynamicTime(0.0), registeringOwnedLight(0)
{
    // Retail builds the merger (0x100EF229), the static collector (0x100EF248), the dynamic
    // collector (0x100EF267), clears +0xBC, builds the saved record (0x100EF28C) and clears +0xC4.
    staticCollectorConstruct(*this);
    dynamicCollectorConstruct(*this);
}

// PE 0x100EFBA0. Runs at exit, as the retail atexit thunk does: clears the registration table and
// the owned lights (PE 0x100EF840: every wrapper is unregistered and destroyed, releasing its
// LightSource) and the transient state (PE 0x100EFAF0); the members are then destroyed in the
// retail delete order (see the struct).
XpCoordinator::~XpCoordinator()
{
    xpClearCoordinator();
    resetXpTransientState();
}

// PE 0x100EFB90: unreachable in 1.50: no code or data references
// Clears the coordinator (PE 0x100EF840), then resets the transient state (PE 0x100EFAF0).
void xpClearAndResetCoordinator()
{
    xpClearCoordinator();
    resetXpTransientState();
}

// PE 0x100EFB50. Resets transient grouping state, updates all wrappers and both capped
// collectors, emits geometry->LightSource pairs into coordinator +0xA4, then finalizes the
// LightMerger (+0xB0). The merger's input map has no producer in the shipped image, so the
// finalizer only clears its dirty byte; it does not consume or clear the +0xA4 pairs.
void prepareLightFrame(double time)
{
    resetXpTransientState();
    updateXpCoordinator(time);
    buildXpGeometryPairs();
    g_xpCoordinator.merger.finalize();
}

// PE 0x100EEED0. Appends the instance's associations without clearing prior entries.
void xpGatherDebugLights(void* geometry, std::vector<world::LightSource*>* out)
{
    auto cursor = g_xpCoordinator.geometryPairs.find(geometry); // PE 0x100A5850
    if (cursor != g_xpCoordinator.geometryPairs.end() && cursor->first == geometry)
    {
        do
        {
            if (cursor == g_xpCoordinator.geometryPairs.end())
                return;
            out->push_back(cursor->second);
            ++cursor;
#if defined(_MSC_VER) && defined(_M_IX86)
            // Shipped defect: the sentinel's key is read before the next iteration's end test.
        } while (cursor->first == geometry);
#else
            // [CHOSEN, not recovered] Native STL sentinels have no value payload. Under the
            // native-container port (issue #7), test end before accessing that absent key.
        } while (cursor != g_xpCoordinator.geometryPairs.end() && cursor->first == geometry);
#endif
    }
}

// PE 0x100EEA80: unreachable in 1.50: no code or data references. Allocates the 0x38-byte wrapper
// for `source` and constructs it with constructXpLight (PE 0x1018FF40); a failed allocation answers
// null. [INFERRED] name.
XpLight* xpNewLightWrapper(world::LightSource* source)
{
    void* const storage = ::renddx9abi::engineAlloc(sizeof(XpLight));
    if (!storage)
        return 0;
    constructXpLight(static_cast<XpLight*>(storage), source);
    return static_cast<XpLight*>(storage);
}

IBase* __fastcall createLightManager(unsigned int iid, IBase* outer)
{
    if (outer)
        return 0;

    LightManager* const self = static_cast<LightManager*>(
        ::renddx9abi::engineAlloc(sizeof(LightManagerData)));
    if (self)
        constructLightManager(self);
    void* const itf = self->LightManager::queryInterface(iid);
    if (!itf)
        self->LightManager::release();
    return static_cast<IBase*>(itf);
}

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

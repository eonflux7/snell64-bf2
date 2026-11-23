
// Retail renderer console bindings and native field/dispatch adapters.
// [INFERRED] companion source placement. Availability follows the owning renderer or
// post-processing singleton; only the bindings declared below are published.

#ifdef BF2_IMAGE_RENDDX9
#include "../../RendDX9Abi/PluginAbi.h"
#include "../Core/ConsoleObjectMacro.h"
#include "../Core/ConsoleObjects.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../Core/IBase.h"
#include "../Core/StringUtil.h"         // stringToFloat (RendDX9.dll copy 0x1007B290)
#include "../Core/Types.h"
#include "../Math/Vec3.h"
#include "../RendMisc/IRenderer.h"
#include "RendDX9Base.h"
#include "QueryManager.h"
#include "PostProductionFrameWork.h"
#include "UiLayer.h"   // [UI] renderer.uiMenuKey
#include "../Geom/Terrain.h"              // the water properties' target, geom::terrain() (dword_1024B930)
#include "../Geom/BundledMeshStateManager.h"
#include "../GeomMisc/FSQuadDrawer.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dice {
namespace hfe {
namespace geom {
extern IBase* g_StaticMeshRenderer;          // PE 0x1024B8F8, ModuleGeom::getSingletonList
extern IBase* g_SkinnedMeshRenderer;         // PE 0x1024B8FC
extern IBase* g_RoadCompiledRenderer;        // PE 0x1024B90C
unsigned char& roadCompiledRendererConsoleDisableFade(void* receiver);
float& roadCompiledRendererConsoleDepthBias(void* receiver);
float& roadCompiledRendererConsoleSlopeScaleDepthBias(void* receiver);
float& roadCompiledRendererConsoleLodDistanceModifier(void* receiver);
void readRendererConsoleVector(std::istream& in, Vec3& value);
void readRendererConsoleVector(std::istream& in, Vec4& value);
}
namespace dx9rend {

extern IBase* g_PostProductionFrameWorkOwner;     // BF2Render.cpp - ds:0x1023F7C0
void* postProductionFrameWorkInterface(void* owner);
extern void*  g_rendererBase;                     // RendDX9Init.cpp - dword_1023B970
extern IBase* g_Renderer;                         // BF2Render.cpp - dword_1023D098
extern void*  g_textureManager;                    // TextureManager.cpp - dword_1023B974
extern void*  g_renderTargetSystem;                // RenderQueues.cpp - mode selector 0x1023F3D8

// TextureManager.cpp - the customTextureSuffix arms (TextureManager::customTextureSuffix) over the
// manager's +0xBC suffix string and its +0xD8 pending-refresh byte.
const std::string& textureManagerGetCustomTextureSuffix();
void textureManagerSetCustomTextureSuffix(const std::string& suffix);

// BF2Render.cpp - Renderer::makeTopWorldScreen (0x1002C5F0) and the two forced-lod
// setters (0x1002D480 / 0x1002D4A0) with the renderer dwords their GET arms read (+0x7DC / +0x7E0).
void rendererMakeTopWorldScreen(unsigned int resolution, renddx9abi::HostString name,
                                Vec3 position, float orthoExtent);
void rendererSetSkinnedMeshLod(int lod);
void rendererSetBundledMeshLod(int lod);
int  rendererGetSkinnedMeshLod();
int  rendererGetBundledMeshLod();
// RendDX9Draw.cpp - the screenshot directory accessors (0x10004FB0 / 0x10004FA0) and the
// screenshot request (0x10004F70) over the base strings +0x3B0 / +0x3CC.
const std::string& rendererGetScreenDumpPath(void* renderer);
void rendererSetScreenDumpPath(void* renderer, const std::string& path);
void rendererRequestScreenDump(void* renderer, const std::string& name);
// BF2Render.cpp - drawHud's arms (0x1005CCC0) over the renderer byte +0x6B1.
bool rendererGetDrawHud();
void rendererSetDrawHud(bool on);
int rendererGetDrawFps();
// BF2Render.cpp - the draw* switch bytes, by retail renderer offset.
bool rendererGetRenderSwitch(unsigned int retailOffset);
void rendererSetRenderSwitch(unsigned int retailOffset, bool on);
// ShaderManager.cpp - the shadermanager console object's arms over g_shaderManager.
extern void* g_shaderManager;                     // ShaderManager.cpp - dword_1023B97C
void shaderManagerReloadAllEffects(void* shaderManager);
void shaderManagerConsoleReloadEffect(const std::string& name);
void shaderManagerListEffects();
bool shaderManagerGetConsoleSwitch(int which);
void shaderManagerSetConsoleSwitch(int which, bool on);
// BF2Render.cpp - the three global mesh LOD distance scales (+0x8B0 / +0x8B4 / +0x8B8;
// which = 0 static, 1 bundled, 2 skinned).
float rendererGetGlobalLodDistanceScale(int which);
void  rendererSetGlobalLodDistanceScale(int which, float value);
void rendererSetDrawFps(int value);

// A void command taking zero or one argument (the generated shape of 0x100531D0 / 0x100532B0):
// both arms clear the return flag; the zero-argument arm passes argZeroExpr. The receiver is the
// renderer base. Core/ConsoleObjectMacro.h has no void 0..1 form.
#define BF2_RENDERER_CONSOLE_METHOD_0_1_VOID(fnName, target, activeExpr, A0, argZeroExpr, listTag)  \
namespace {                                                                                        \
class BF2_CC_NAME : public ::dice::hfe::ConsoleObjectBaseImpl                                      \
{                                                                                                  \
    BF2_CC_HEAD(fnName, 0, 0, 1, "void", false)                                                    \
            m_argTypes[0] = #A0;                                                                   \
    BF2_CC_TAIL(1, listTag)                                                                        \
    virtual bool isObjectActive() const { return (activeExpr); }                                   \
    virtual void executeObjectMethod()                                                             \
    {                                                                                              \
        if (m_argCount == 0)                                                                       \
        {                                                                                          \
            m_hasReturnValue = false;                                                              \
            target(g_rendererBase, (argZeroExpr));                                                 \
        }                                                                                          \
        else if (m_argCount == 1)                                                                  \
        {                                                                                          \
            m_hasReturnValue = false;                                                              \
            target(g_rendererBase, BF2_CC_ARG(A0, 0));                                             \
        }                                                                                          \
    }                                                                                              \
    virtual void setArgFromString(int index, const std::string& value)                             \
    {                                                                                              \
        BF2_CC_CONVERT(A0, 1)                                                                      \
    }                                                                                              \
    BF2_CC_NO_RETURN_AS_STRING                                                                     \
    BF2_CC_COMMON_HOOKS                                                                            \
};                                                                                                 \
static BF2_CC_NAME BF2_CC_INSTANCE;                                                                \
}

// PE 0x10056070 / 0x10056F10: type 3, exactly one string argument, and a constructed string
// return slot even though the executor only has a SET arm.
#define BF2_RENDERER_WRITEONLY(fnName, target)                                                      \
namespace {                                                                                       \
class BF2_CC_NAME : public ::dice::hfe::ConsoleObjectBaseImpl                                       \
{                                                                                                 \
    BF2_CC_HEAD(fnName, 3, 1, 1, "std::string", true)                                               \
        m_argTypes[0] = "std::string";                                                             \
    BF2_CC_TAIL(1, kRendererConsoleTag)                                                             \
    virtual bool isObjectActive() const { return g_renderTargetSystem != 0; }                      \
    virtual void executeObjectMethod()                                                            \
    {                                                                                             \
        if (m_argCount == 1)                                                                       \
        {                                                                                         \
            m_hasReturnValue = false;                                                             \
            target(BF2_CC_ARG(std::string, 0));                                                    \
        }                                                                                         \
    }                                                                                             \
    virtual void setArgFromString(int index, const std::string& value)                            \
    {                                                                                             \
        BF2_CC_CONVERT(std::string, 1)                                                             \
    }                                                                                             \
    BF2_CC_RETURN_AS_STRING                                                                       \
    BF2_CC_COMMON_HOOKS                                                                           \
private:                                                                                          \
    std::string m_returnValue;                                                                    \
};                                                                                                \
static BF2_CC_NAME BF2_CC_INSTANCE;                                                                \
}

namespace {

// Stands in for the original's file-scope list (the static at 0x10241668).
const char kRendererConsoleTag[] = "dx9rend.renderer";


// Boolean presentation properties: zero arguments reads, one writes.
// Require an active renderer; changing vsync resets its device.

bool getVSync()                     { return rendererGetVSync(g_rendererBase); }
void setVSync(bool value)           { rendererSetVSync(g_rendererBase, value); }
bool getPresentAsync()              { return rendererGetPresentAsync(g_rendererBase); }
void setPresentAsync(bool value)    { rendererSetPresentAsync(g_rendererBase, value); }
bool getPresentSpinIfBusy()         { return rendererGetPresentSpinIfBusy(g_rendererBase); }
void setPresentSpinIfBusy(bool value) { rendererSetPresentSpinIfBusy(g_rendererBase, value); }
void setScreenDumpPath(const std::string& value) { rendererSetScreenDumpPath(g_rendererBase, value); }

// fogColor - ctor 0x100586B0 (type "Vec3"). Read arm 0x100588D0 -> the stored,
// normalised colour times 255. Write arm 0x10058930 -> divided by 255 on the way in.
Vec3 getFogColor()
{
    Vec3 v;
    postProductionGetFogColor255(&v.x);
    return v;
}

void setFogColor(const Vec3& v)
{
    postProductionSetFogColor255(&v.x);
}

// waterFogColor - ctor 0x10058950 (type "Vec3"). Read arm 0x10058AF1 -> 0x1004F700, write arm
// 0x10058B3D -> 0x1004F690, which stores the one colour into both water-fog colour slots.
Vec3 getWaterFogColor()
{
    Vec3 v;
    postProductionGetWaterFogColor255(&v.x);
    return v;
}

void setWaterFogColor(const Vec3& v)
{
    postProductionSetWaterFogColor255(&v.x);
}

// waterFogStartEndAndBase - ctor 0x10058B50 (type "Vec4"), active with the post-production owner
// (0x10057370). Read arm 0x10058D50 -> 0x1004F730 (range B as stored), write arm -> 0x1004FD50
// (both ranges, then the terrain's lighting-dirty byte).
Vec4 getWaterFogStartEndAndBase()
{
    Vec4 v;
    postProductionGetWaterFogStartEndAndBase(&v.x);
    return v;
}

void setWaterFogStartEndAndBase(const Vec4& v)
{
    postProductionSetWaterFogStartEndAndBase(&v.x);
}

float getGlobalStaticMeshLodDistanceScale()  { return rendererGetGlobalLodDistanceScale(0); }
void  setGlobalStaticMeshLodDistanceScale(float v)  { rendererSetGlobalLodDistanceScale(0, v); }
float getGlobalBundleMeshLodDistanceScale()  { return rendererGetGlobalLodDistanceScale(1); }
void  setGlobalBundleMeshLodDistanceScale(float v)  { rendererSetGlobalLodDistanceScale(1, v); }
float getGlobalSkinnedMeshLodDistanceScale() { return rendererGetGlobalLodDistanceScale(2); }
void  setGlobalSkinnedMeshLodDistanceScale(float v) { rendererSetGlobalLodDistanceScale(2, v); }

Vec3 getTVColor()
{
    Vec3 value;
    postProductionGetTVColor(&value.x);
    return value;
}

void setTVColor(const Vec3& value) { postProductionSetTVColor(&value.x); }

// PE 0x10056380 / 0x10056550; the post-production interface reads the same renderer fields.
float getNightVisionMaxPower() { return rendererTailFloat(0x73C); }
void setNightVisionMaxPower(float value) { rendererTailFloat(0x73C) = value; }
float getNightVisionRegenRPS() { return rendererTailFloat(0x740); }
void setNightVisionRegenRPS(float value) { rendererTailFloat(0x740) = value; }

// PE 0x1005AAE0, Renderer +0xA8/+0xA4 (the setter caps the fence-ring length at five).
typedef unsigned int uint;
uint getRenderAheadLimit() { return static_cast<IRenderer*>(static_cast<void*>(g_Renderer))->getRenderAheadLimit(); }
void setRenderAheadLimit(uint value) { static_cast<IRenderer*>(static_cast<void*>(g_Renderer))->setRenderAheadLimit(value); }

#define BF2_RENDERER_FLOAT_ACCESSORS(name, offset) \
    float get_##name() { return rendererTailFloat(offset); } \
    void set_##name(float value) { rendererTailFloat(offset) = value; }
// PE 0x10059AA0 / 0x10059C40 / 0x10059DE0 / 0x10059F80.
BF2_RENDERER_FLOAT_ACCESSORS(tweakNearPlane, 0x590)
BF2_RENDERER_FLOAT_ACCESSORS(tweakNearPlane1p, 0x594)
BF2_RENDERER_FLOAT_ACCESSORS(tweakFarPlane1p, 0x598)
BF2_RENDERER_FLOAT_ACCESSORS(coronaFadeSpeed, 0x59C)
// PE 0x1005A2C0.
BF2_RENDERER_FLOAT_ACCESSORS(divAccum, 0x58C)
#undef BF2_RENDERER_FLOAT_ACCESSORS

// PE 0x10059760: a negative request leaves the previous skip count unchanged.
int getForceStaticMeshSkipLod() { return rendererTailInt(0x7E4); }
void setForceStaticMeshSkipLod(int value)
{
    if (value >= 0)
        rendererTailInt(0x7E4) = value;
}
// PE 0x10059900: ignore requests outside the inclusive 0..100 interval.
int getForceStaticMeshMaxPolyPercentage() { return rendererTailInt(0x7E8); }
void setForceStaticMeshMaxPolyPercentage(int value)
{
    if (value >= 0 && value <= 100)
        rendererTailInt(0x7E8) = value;
}
// PE 0x1005A120 / 0x1005A460.
bool getAllowOnDemandPostProcessing() { return rendererTailByte(0x5A1) != 0; }
void setAllowOnDemandPostProcessing(bool value) { rendererTailByte(0x5A1) = value; }
bool getAmbientSun() { return rendererRenderSwitch(0x69C) != 0; }
void setAmbientSun(bool value) { rendererRenderSwitch(0x69C) = value; }

// PE 0x1005AEA0/0x1005B030, 0x1005B250/0x1005B3E0, 0x1005B620/0x1005B7B0.
// Invalid indices read as zero and leave a write unchanged; distances are unrestricted.

// PE 0x1005BB80, GET via +0xAC; SET inlines 0x10028310, without mounting another shader archive.
bool getIsNightMap() { return static_cast<IRenderer*>(static_cast<void*>(g_Renderer))->isExpansionPack(); }
void setIsNightMap(bool value)
{
    unsigned char& field = rendererTailByte(0x764);
    if (field != static_cast<unsigned char>(value))
        field = value;
}
// PE 0x1005D1F0 / 0x1005D580 / 0x1005D800.
bool getDrawWatermark() { return rendererTailByte(0x6DD) != 0; }
void setDrawWatermark(bool value) { rendererTailByte(0x6DD) = value; }
float getWatermarkScale() { return rendererTailFloat(0x6FC); }
void setWatermarkScale(float value) { rendererTailFloat(0x6FC) = value; }
Vec2 getWatermarkPos()
{
    Vec2 value;
    value.x = rendererTailFloat(0x700);
    value.y = rendererTailFloat(0x704);
    return value;
}
void setWatermarkPos(const Vec2& value)
{
    rendererTailFloat(0x700) = value.x;
    rendererTailFloat(0x704) = value.y;
}

// PE 0x1007B260: _gcvt(float promoted to double, 12, shared buffer at 0x10247C0C).
std::string consoleFloatToString(float value)
{
    static char buffer[20]; // [INFERRED] extent up to the console-default globals at 0x10247C20.
#if defined(_MSC_VER)
    return std::string(::_gcvt(static_cast<double>(value), 12, buffer));
#else
    // [CHOSEN, not recovered] The native CRT spells the same conversion gcvt.
    return std::string(::gcvt(static_cast<double>(value), 12, buffer));
#endif
}
// PE 0x1004E320, called by 0x1004E040/0x1004E0A0 after copying the drawer settings.
std::string getFakeHDRWeights(unsigned int first)
{
    const geom::FSQuadDrawerSettings settings = geom::fsQuadDrawerGetSettings();
    const std::string separator(",");
    std::string result;
    for (unsigned int i = 0; i < 9; ++i)
    {
        if (i != 0)
            result += separator;
        result += consoleFloatToString(settings.weights[first + i]);
    }
    return result;
}
// PE 0x1004E710: split on commas, convert each token with atof, and omit a trailing empty token.
std::vector<float> parseFakeHDRWeights(const std::string& value, const std::string& separator)
{
    std::vector<float> result;
    std::string remaining(value);
    while (!remaining.empty())
    {
        const std::string::size_type position = remaining.find(separator.c_str(), 0, separator.size());
        {
            const std::string token = remaining.substr(0, position);
            // The string-to-number helper (Core/StringUtil.cpp, RendDX9.dll copy 0x1007B290).
            result.push_back(::dice::hfe::stringToFloat(token));
        }
        if (position == std::string::npos)
            break;
        remaining = remaining.substr(position + 1, std::string::npos);
    }
    return result;
}
// PE 0x1004E180/0x1004E230: only nine parsed weights replace the selected half; rebuild all taps.
void setFakeHDRWeightsHalf(const std::string& value, unsigned int first)
{
    geom::FSQuadDrawerSettings settings = geom::fsQuadDrawerGetSettings();
    const std::vector<float> weights = parseFakeHDRWeights(value, std::string(","));
    if (weights.size() == 9)
    {
        std::copy(weights.begin(), weights.end(), settings.weights + first);
        geom::fsQuadDrawerSetSettings(settings);
    }
}
// PE 0x1004E040 (the executors, cited at the bindings, copy their string argument before forwarding)
std::string getFakeHDRHorizWeights() { return getFakeHDRWeights(0); }
void setFakeHDRHorizWeights(std::string value) { setFakeHDRWeightsHalf(value, 0); }
// PE 0x1004E0A0
std::string getFakeHDRVertWeights() { return getFakeHDRWeights(9); }
void setFakeHDRVertWeights(std::string value) { setFakeHDRWeightsHalf(value, 9); }
// PE 0x1004E2E0 (the fakeHDRWeights executor forwards to it): two independent parses/settings
// copies/cache rebuilds, in order.
void setFakeHDRWeights(std::string value)
{
    setFakeHDRWeightsHalf(value, 0);
    setFakeHDRWeightsHalf(value, 9);
}
// PE 0x10057220 -> 0x1004E010/0x1004DFD0, the drawer's settings +0x48 high-pass gate.
// PE 0x1004E010
float getFakeHDRPassGate() { return geom::fsQuadDrawerGetSettings().highPassGate; }
// PE 0x1004DFD0
void setFakeHDRPassGate(float value)
{
    geom::FSQuadDrawerSettings settings = geom::fsQuadDrawerGetSettings();
    settings.highPassGate = value;
    geom::fsQuadDrawerSetSettings(settings);
}
// PE 0x10056160 -> 0x1004E020: one argument copy and the preset's guarded gradient update.
void setNightVisionGradient(std::string value) { postProductionSetNightVisionGradient(value); }

struct ConsoleClearColorValue { int x, y, z; };
static_assert(sizeof(ConsoleClearColorValue) == 12, "clearColor contains three 32-bit integers");
// PE 0x10009E70/0x10009E90: the last twelve bytes of the named, pointer-free statistics window.
unsigned char* consoleClearColorBytes()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->renderStats + (0x390 - 0x108);
}
ConsoleClearColorValue getConsoleClearColor()
{
    ConsoleClearColorValue value;
    std::memcpy(&value, consoleClearColorBytes(), sizeof value);
    return value;
}
void setConsoleClearColor(const ConsoleClearColorValue& value)
{
    std::memcpy(consoleClearColorBytes(), &value, sizeof value);
}
// PE 0x10009F50, 0x10009F60: shared field-read implementation (mov eax, [ecx + 0x518]).
int getXpPerfFlags()
{
    int value;
    std::memcpy(&value, &static_cast<RendDX9Base*>(g_rendererBase)->performanceFlags, sizeof value);
    return value;
}
// PE 0x10009F10: stores the argument at +0x518.
void setXpPerfFlags(int value)
{
    static_cast<RendDX9Base*>(g_rendererBase)->performanceFlags = static_cast<unsigned int>(value);
}
// PE 0x10009F20.
void setXpPerfFlags2(int value)
{
    RendDX9Base* const base = static_cast<RendDX9Base*>(g_rendererBase);
    if (value == -1)
        base->performanceFlags = 0;
    else
    {
#if defined(_M_IX86)
        base->performanceFlags = 1u << value;
#else
        // [CHOSEN, not recovered] Reproduce the x86 SHL's five-bit count mask without C++ UB.
        base->performanceFlags = 1u << (static_cast<unsigned int>(value) & 31u);
#endif
    }
}
// PE 0x10053500
int getOcclusionTerrainMaxFrames()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->occlusionTerrainMaxFrames;
}
// PE 0x10053500: upper-only signed cap; negative ages are retained.
void setOcclusionTerrainMaxFrames(int value)
{
    static_cast<RendDX9Base*>(g_rendererBase)->occlusionTerrainMaxFrames = value > 10 ? 10 : value;
}
// PE 0x100536A0
int getOcclusionObjectsMaxFrames()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->occlusionObjectsMaxFrames;
}
// PE 0x100536A0: the same upper-only signed cap.
void setOcclusionObjectsMaxFrames(int value)
{
    static_cast<RendDX9Base*>(g_rendererBase)->occlusionObjectsMaxFrames = value > 10 ? 10 : value;
}
// PE 0x10053850 -> 0x10004250
void setEnableHWOcclusion(bool value) { queryManagerSetHWOcclusion(g_queryManager, value); }

// PE 0x10053A00: direct access to the alpha-to-coverage byte, including writes on unsupported HW.
bool getAllowA2M() { return static_cast<RendDX9Base*>(g_rendererBase)->hasAlphaToCoverage != 0; }
void setAllowA2M(bool value) { static_cast<RendDX9Base*>(g_rendererBase)->hasAlphaToCoverage = value; }
// PE 0x10053AE0: Renderer +0x68, without an argument or return value.
void consoleStopRenderMovie() { static_cast<IRenderer*>(g_rendererBase)->slot_0x68(); }
// PE 0x10054140: activity follows Renderer, while the manager is deliberately unguarded.
bool getDraw1pZOnly() { return geom::g_bundledMeshStateManager->m_enabled9 != 0; }
void setDraw1pZOnly(bool value) { geom::g_bundledMeshStateManager->m_enabled9 = value; }

// PE 0x10058630, Vec4 copied through the published post-production interface.
Vec4 getAmbientIlluminationRange()
{
    Vec4 value;
    postProductionGetAmbientIlluminationRange(&value.x);
    return value;
}
void setAmbientIlluminationRange(const Vec4& value)
{
    postProductionSetAmbientIlluminationRange(&value.x);
}



// The six water properties (ctors 0x1005BBE0, 0x1005BF40, 0x1005C110, 0x1005C440, 0x1005C710,
// 0x1005C9A0; vtables 0x101E6328, 0x101E63B8, 0x101E6450, 0x101E64E8, 0x101E6580, 0x101E6610), all
// active while the renderer exists (0x1005B930). Each arm forwards to the Terrain singleton
// (dword_1024B930) when there is one: a read without a terrain answers zero (the empty string for
// waterNormalMap), a write without one does nothing. Water.con sets them when a level loads; the
// water shaders read the values through updateWaterConstants and the terrain material block.

// waterScroll ("Vec2") - executor 0x1005BE40: get 0x1005BEB0 (Terrain slot +0x88), set
// 0x1005BF10 (slot +0x8C).
// PE 0x1005BEB0.
Vec2 getWaterScroll()
{
    Vec2 v;
    if (geom::Terrain* const t = geom::terrain())
        return *t->getWaterScroll(&v);
    v.x = 0.0f; v.y = 0.0f;
    return v;
}
// PE 0x1005BF10.
void setWaterScroll(Vec2 v)
{
    if (geom::Terrain* const t = geom::terrain())
        t->setWaterScroll(v.x, v.y);
}

// waterAnimSpeed ("float") - executor 0x1005C090, both arms inline: slot +0x90 / +0x94, the read
// without a terrain answers flt_101DDE64 (0.0f).
float getWaterAnimSpeed()
{
    geom::Terrain* const t = geom::terrain();
    return t ? t->getWaterAnimSpeed() : 0.0f;
}
void setWaterAnimSpeed(float v)
{
    if (geom::Terrain* const t = geom::terrain())
        t->setWaterAnimSpeed(v);
}

// waterNormalMap ("std::string") - executor 0x1005C200: get 0x1005C290 (slot +0x98, else
// std::string("") from 0x101DC333), set 0x1005C310 (slot +0x9C, the name by value; the terrain
// reruns setupWater).
// PE 0x1005C290.
std::string getWaterNormalMap()
{
    geom::Terrain* const t = geom::terrain();
    return t ? t->getWaterNormalMap() : std::string("");
}
// PE 0x1005C310.
void setWaterNormalMap(std::string name)
{
    if (geom::terrain())
        geom::terrain()->setWaterNormalMap(name);
}

// waterSpecularColor ("Vec4") - executor 0x1005C5D0: get 0x1005C670 (slot +0xA0), set
// 0x1005C6D0 (slot +0xA4); the colour's alpha is the specular strength the water shaders use.
// PE 0x1005C670.
Vec4 getWaterSpecularColor()
{
    Vec4 v;
    if (geom::Terrain* const t = geom::terrain())
        return *t->getWaterSpecularColor(&v);
    v.x = 0.0f; v.y = 0.0f; v.z = 0.0f; v.w = 0.0f;
    return v;
}
// PE 0x1005C6D0.
void setWaterSpecularColor(Vec4 v)
{
    if (geom::Terrain* const t = geom::terrain())
        t->setWaterSpecularColor(v.x, v.y, v.z, v.w);
}

// waterColor ("Vec3") - executor 0x1005C890: get 0x1005C910, set 0x1005C960, which call the
// non-virtual Terrain::getWaterColor / setWaterColor (0x100DB980 / 0x100DB9A0).
// PE 0x1005C910.
Vec3 getWaterColor()
{
    if (geom::Terrain* const t = geom::terrain())
        return t->getWaterColor();
    Vec3 v;
    v.x = 0.0f; v.y = 0.0f; v.z = 0.0f;
    return v;
}
// PE 0x1005C960.
void setWaterColor(Vec3 v)
{
    if (geom::Terrain* const t = geom::terrain())
        t->setWaterColor(v);
}

// waterSpecularPower ("float") - executor 0x1005CAF0, both arms inline: slot +0xA8 / +0xAC, the
// read without a terrain answers 0.0f.
float getWaterSpecularPower()
{
    geom::Terrain* const t = geom::terrain();
    return t ? t->getWaterSpecularPower() : 0.0f;
}
void setWaterSpecularPower(float v)
{
    if (geom::Terrain* const t = geom::terrain())
        t->setWaterSpecularPower(v);
}

// Native capture adapter. The renderer's compatibility path captures at a fixed 2048 square.

void consoleMakeTopWorldScreen(U32 resolution, const std::string& name, const Vec3& position,
                               float orthoExtent)
{
    rendererMakeTopWorldScreen(resolution, renddx9abi::HostString(name), position, orthoExtent);
}

// The explicit manager tables have no compiler-declared setter interface. Preserve their
// dispatch after the renderer byte store. PE StaticMeshRenderer 0x1021C6CC +0x18 and
// RoadCompiledRenderer 0x1021DAD4 +0x18 -> 0x10121A00; SkinnedMeshRenderer 0x1021C928 +0x10
// -> 0x101240A0. The native signatures match the existing RD9_THUNK table entries.
template<class Arg> void consoleCallSetter(void* manager, unsigned int slot, Arg value)
{
    typedef void (RD9_THUNK* Fn)(void* RD9_THUNK_EDX, Arg);
    const void* const* const table = *static_cast<const void* const* const*>(manager);
    reinterpret_cast<Fn>(const_cast<void*>(table[slot / 4]))(manager RD9_THUNK_ARG, value);
}

// PE 0x10057C10: the post-production interface's +0x08/+0x04 pair, rather than direct fields.
float getTVInterference()
{
    void* const self = postProductionFrameWorkInterface(g_PostProductionFrameWorkOwner);
    const void* const* const table = *static_cast<const void* const* const*>(self);
    typedef float (RD9_THUNK* Fn)(void* RD9_THUNK_EDX);
    return reinterpret_cast<Fn>(const_cast<void*>(table[0x08 / 4]))(self RD9_THUNK_ARG);
}
void setTVInterference(float value)
{
    void* const self = postProductionFrameWorkInterface(g_PostProductionFrameWorkOwner);
#if defined(_M_IX86)
    unsigned int bits;
    std::memcpy(&bits, &value, sizeof bits);
    consoleCallSetter<unsigned int>(self, 0x04, bits);
#else
    // [CHOSEN, not recovered] The native table's FloatArg is float (passed in an FP register).
    consoleCallSetter<float>(self, 0x04, value);
#endif
}

// PE 0x10053F90: Renderer +0x6A1, then BundledMeshStateManager +0x10, unguarded.
bool get_drawBundledMeshes() { return rendererGetRenderSwitch(0x6A1); }
void set_drawBundledMeshes(bool value)
{
    rendererSetRenderSwitch(0x6A1, value);
    geom::g_bundledMeshStateManager->setEnabled(value);
}
// PE 0x10054610: Renderer +0x6A2, then StaticMeshRenderer +0x18, unguarded.
bool get_drawStaticMeshes() { return rendererGetRenderSwitch(0x6A2); }
void set_drawStaticMeshes(bool value)
{
    rendererSetRenderSwitch(0x6A2, value);
    consoleCallSetter<int>(geom::g_StaticMeshRenderer, 0x18, value);
}
// PE 0x100547C0: Renderer +0x6A3, then SkinnedMeshRenderer +0x10, unguarded.
bool get_drawSkinnedMeshes() { return rendererGetRenderSwitch(0x6A3); }
void set_drawSkinnedMeshes(bool value)
{
    rendererSetRenderSwitch(0x6A3, value);
    consoleCallSetter<unsigned char>(geom::g_SkinnedMeshRenderer, 0x10, value);
}
// PE 0x10054D20: Renderer +0x6A4, then RoadCompiledRenderer +0x18, unguarded.
bool get_drawRoads() { return rendererGetRenderSwitch(0x6A4); }
void set_drawRoads(bool value)
{
    rendererSetRenderSwitch(0x6A4, value);
    consoleCallSetter<int>(geom::g_RoadCompiledRenderer, 0x18, value);
}

// The other draw* switches only read/store their renderer byte (activity 0x1005B930).
#define BF2_RENDERER_SWITCH_ACCESSORS(name, offset)                                                 \
    bool get_##name() { return rendererGetRenderSwitch(offset); }                                   \
    void set_##name(bool on) { rendererSetRenderSwitch(offset, on); }
BF2_RENDERER_SWITCH_ACCESSORS(drawObjects, 0x69Du)
BF2_RENDERER_SWITCH_ACCESSORS(drawDeferred, 0x69Eu)
BF2_RENDERER_SWITCH_ACCESSORS(draw1pMeshes, 0x5A0u)
BF2_RENDERER_SWITCH_ACCESSORS(drawParticles, 0x69Fu)
BF2_RENDERER_SWITCH_ACCESSORS(drawSkyDome, 0x6B2u)
BF2_RENDERER_SWITCH_ACCESSORS(drawSunFlare, 0x6B3u)
BF2_RENDERER_SWITCH_ACCESSORS(drawTerrain, 0x6A5u)
BF2_RENDERER_SWITCH_ACCESSORS(drawUndergrowth, 0x6A6u)
BF2_RENDERER_SWITCH_ACCESSORS(drawOvergrowth, 0x6A7u)
BF2_RENDERER_SWITCH_ACCESSORS(drawNametags, 0x6A8u)
BF2_RENDERER_SWITCH_ACCESSORS(drawTrees, 0x6A9u)
BF2_RENDERER_SWITCH_ACCESSORS(drawPostProduction, 0x6B5u)
BF2_RENDERER_SWITCH_ACCESSORS(drawGraphs, 0x6B0u)
BF2_RENDERER_SWITCH_ACCESSORS(drawConsole, 0x6B4u)
#undef BF2_RENDERER_SWITCH_ACCESSORS
#define BF2_RENDERER_SWITCH(name)                                                                   \
    BF2_CONSOLE_PROPERTY_FN(name, bool, g_Renderer != 0, get_##name(), set_##name, kRendererConsoleTag)

template<class T> void readRendererProperty(std::istream& in, T& value) { in >> value; }
void readRendererProperty(std::istream& in, Vec3& value) { geom::readRendererConsoleVector(in, value); }
void readRendererProperty(std::istream& in, Vec4& value) { geom::readRendererConsoleVector(in, value); }
// PE 0x1005BDF0, source-level vector extraction with a reused component temporary.
void readRendererProperty(std::istream& in, Vec2& value)
{
    float component;
    char separator;
    in >> component >> separator;
    value.x = component;
    in >> component;
    value.y = component;
}

// PE 0x10051900. The shared helper the shipped property constructors call for m_argNeedsConversion
// (the type name arrives as a by-value std::string): 0 for "std::string", else -1.
int rendererArgNeedsConversion(const std::string& typeName)
{
    return typeName == "std::string" ? 0 : -1;
}

// [CHOSEN, not recovered] Local generator spelling; retail registration precedes
// argument metadata, and extraction writes persistent storage even after a failed read.
#define BF2_RENDERER_PROPERTY(name, T, initClause, activeExpr, getExpr, setCall)          \
class BF2_CC_NAME : public ConsoleObjectBaseImpl                                        \
{                                                                                      \
public:                                                                                \
    BF2_CC_NAME() initClause                                                            \
    {                                                                                  \
        m_objectName = consoleDefaultName();                                            \
        m_functionName = #name;                                                         \
        m_isDynamic = true;                                                             \
        m_type = 1;                                                                    \
        m_access = consoleDefaultAccess();                                              \
        m_category = consoleDefaultCategory();                                          \
        m_minNoArgs = 0;                                                                \
        m_maxNoArgs = 1;                                                                \
        m_returnType = #T;                                                              \
        m_hasReturnValue = false;                                                       \
        consoleBindingList(kRendererConsoleTag).push_back(this);                        \
        m_argTypes[0] = #T;                                                             \
        m_argNeedsConversion[0] = rendererArgNeedsConversion(std::string(#T));          \
    }                                                                                  \
    virtual bool isObjectActive() const { return (activeExpr); }                        \
    virtual void executeObjectMethod()                                                 \
    {                                                                                  \
        if (m_argCount == 0)                                                            \
        {                                                                              \
            m_hasReturnValue = true;                                                   \
            m_returnValue = (getExpr);                                                 \
        }                                                                              \
        else if (m_argCount == 1)                                                       \
        {                                                                              \
            const T value = BF2_CC_ARG(T, 0);                                           \
            m_hasReturnValue = false;                                                  \
            setCall(value);                                                            \
        }                                                                              \
    }                                                                                  \
    virtual void setArgFromString(int index, const std::string& value)                  \
    {                                                                                  \
        if (index == 1)                                                                 \
        {                                                                              \
            static T stored;                                                           \
            std::istringstream in(value);                                              \
            readRendererProperty(in, stored);                                          \
            m_args[0] = reinterpret_cast<std::string*>(&stored);                        \
        }                                                                              \
    }                                                                                  \
    BF2_CC_RETURN_AS_STRING                                                             \
    BF2_CC_COMMON_HOOKS                                                                 \
private:                                                                               \
    T m_returnValue;                                                                    \
};                                                                                     \
static BF2_CC_NAME BF2_CC_INSTANCE;

BF2_CONSOLE_OBJECT("renderer", 2, 0)

// in the shipped static-initialiser order, ahead of fogColor.
// PE 0x10052400, 0x10052500, 0x10052560: vsync constructor, converter, executor.
// PE 0x10051AA0: the constructors' push onto this TU's console list (0x10241668), which every
// constructor inlines (consoleBindingList(kRendererConsoleTag).push_back(this)); the outlined
// copy is unreachable in 1.50: no code or data references.
BF2_CONSOLE_PROPERTY_FN(vsync, bool, g_rendererBase != 0,
                        getVSync(), setVSync, kRendererConsoleTag)
// PE 0x100525E0, 0x100526D0, 0x10052730: presentAsync constructor, converter, executor.
BF2_CONSOLE_PROPERTY_FN(presentAsync, bool, g_rendererBase != 0,
                        getPresentAsync(), setPresentAsync, kRendererConsoleTag)
// PE 0x10052790, 0x10052880, 0x100528E0: presentSpinIfBusy constructor, converter, executor.
BF2_CONSOLE_PROPERTY_FN(presentSpinIfBusy, bool, g_rendererBase != 0,
                        getPresentSpinIfBusy(), setPresentSpinIfBusy, kRendererConsoleTag)

class RendererClearColorBinding : public ConsoleObjectBaseImpl
{
public:
    // PE 0x10052940: constructor (type 1, dynamic, 0..1 "BaseVector3<int>" argument).
    RendererClearColorBinding()
    {
        m_objectName = consoleDefaultName();
        m_access = consoleDefaultAccess();
        m_category = consoleDefaultCategory();
        m_functionName = "clearColor";
        m_type = 1;
        m_minNoArgs = 0;
        m_maxNoArgs = 1;
        m_returnType = "BaseVector3<int>";
        m_hasReturnValue = false;
        m_isDynamic = true;
        m_argTypes[0] = "BaseVector3<int>";
        m_argNeedsConversion[0] = -1;
        consoleBindingList(kRendererConsoleTag).push_back(this);
    }
    virtual bool isObjectActive() const { return g_rendererBase != 0; }
    // PE 0x10052A30: executor.
    virtual void executeObjectMethod()
    {
        if (m_argCount == 0)
        {
            m_hasReturnValue = true;
            m_returnValue = getConsoleClearColor();
        }
        else if (m_argCount == 1)
        {
            const ConsoleClearColorValue value = BF2_CC_ARG(ConsoleClearColorValue, 0);
            m_hasReturnValue = false;
            setConsoleClearColor(value);
        }
    }
    // PE 0x10052B70, 0x10052BD0: converter (guarded static parse target) and the integer-vector extraction.
    virtual void setArgFromString(int index, const std::string& value)
    {
        if (index == 1)
        {
            static ConsoleClearColorValue parsed;
            std::istringstream in(value);
            int component;
            char separator;
            in >> component >> separator;
            parsed.x = component;
            in >> component >> separator;
            parsed.y = component;
            in >> component;
            parsed.z = component;
            m_args[0] = reinterpret_cast<std::string*>(&parsed);
        }
    }
    // PE 0x10052AB0, 0x10052B20: return formatter and the integer-vector insertion ('/' separators).
    virtual std::string getReturnValueAsString() const
    {
        if (!m_hasReturnValue)
            return std::string();
        std::ostringstream out;
        out << m_returnValue.x << '/' << m_returnValue.y << '/' << m_returnValue.z;
        return out.str();
    }
    BF2_CC_COMMON_HOOKS
private:
    ConsoleClearColorValue m_returnValue;
};
static RendererClearColorBinding s_clearColor;

// PE 0x10052C40, 0x10052D30: xpPerfFlags constructor, converter.
BF2_CONSOLE_PROPERTY_FN(xpPerfFlags, int, g_rendererBase != 0,
                        getXpPerfFlags(), setXpPerfFlags, kRendererConsoleTag)
// PE 0x10052DE0, 0x10052ED0, 0x10052F40: xpPerfFlags2 constructor, return formatter, converter.
BF2_CONSOLE_PROPERTY_FN(xpPerfFlags2, int, g_rendererBase != 0,
                        getXpPerfFlags(), setXpPerfFlags2, kRendererConsoleTag)

// screenDumpPath - ctor 0x10052FF0 (type "std::string", dynamic). Read arm 0x100530F0 -> the
// directory getter 0x10004FB0, write arm 0x1005311D -> its setter 0x10004FA0.
// PE 0x100530E0, 0x10053150: screenDumpPath executor, converter.
BF2_CONSOLE_PROPERTY_FN(screenDumpPath, std::string, g_rendererBase != 0,
                        rendererGetScreenDumpPath(g_rendererBase), setScreenDumpPath, kRendererConsoleTag)

// dumpScreen - ctor 0x100531D0 (method, "void", 0..1 "std::string" arguments, not dynamic),
// active while the renderer exists (0x100524F0). execute 0x100532B0: no argument requests an
// automatically numbered capture with the empty name, one argument the named capture.
// PE 0x100532B0, 0x10053330: dumpScreen executor, converter.
BF2_RENDERER_CONSOLE_METHOD_0_1_VOID(dumpScreen, rendererRequestScreenDump, g_rendererBase != 0,
                                     std::string, std::string(""), kRendererConsoleTag)

// PE 0x100533B0, 0x100534A0: occlusionTerrainMaxFrames constructor, converter.
BF2_CONSOLE_PROPERTY_FN(occlusionTerrainMaxFrames, int, g_rendererBase != 0,
                        getOcclusionTerrainMaxFrames(), setOcclusionTerrainMaxFrames, kRendererConsoleTag)
// PE 0x10053550, 0x10053640: occlusionObjectsMaxFrames constructor, converter.
BF2_CONSOLE_PROPERTY_FN(occlusionObjectsMaxFrames, int, g_rendererBase != 0,
                        getOcclusionObjectsMaxFrames(), setOcclusionObjectsMaxFrames, kRendererConsoleTag)
// PE 0x100536F0, 0x100537F0: enableHWOcclusion constructor, converter.
BF2_CONSOLE_PROPERTY_FN(enableHWOcclusion, bool, g_queryManager != 0,
                        queryManagerGetHWOcclusion(g_queryManager), setEnableHWOcclusion, kRendererConsoleTag)

// PE 0x100538B0, 0x100539A0: allowA2M constructor, converter.
BF2_CONSOLE_PROPERTY_FN(allowA2M, bool, g_rendererBase != 0,
                        getAllowA2M(), setAllowA2M, kRendererConsoleTag)
// PE 0x10053A50: stopRenderMovie constructor.
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(stopRenderMovie, consoleStopRenderMovie,
                                   g_rendererBase != 0, kRendererConsoleTag)

// The draw* switches, ctors 0x10053B00..0x10055740 (between stopRenderMovie and makeTopWorldScreen in the
// shipped initialiser), including the BundledMeshStateManager's +0x09 draw1pZOnly byte.
// PE 0x10053B00 (constructor) / 0x10053BF0 (converter).
// PE 0x10053BF0: drawObjects converter.
BF2_RENDERER_SWITCH(drawObjects)
// PE 0x10053CA0 (constructor) / 0x10053D90 (converter).
// PE 0x10053D90: drawDeferred converter.
BF2_RENDERER_SWITCH(drawDeferred)
// PE 0x10053E40, 0x10053F30: drawBundledMeshes constructor, converter.
BF2_RENDERER_SWITCH(drawBundledMeshes)
// PE 0x10053FF0, 0x100540E0: draw1pZOnly constructor, converter.
BF2_CONSOLE_PROPERTY_FN(draw1pZOnly, bool, g_Renderer != 0,
                        getDraw1pZOnly(), setDraw1pZOnly, kRendererConsoleTag)
// PE 0x10054180 (constructor) / 0x10054270 (converter).
// PE 0x10054270: draw1pMeshes converter.
BF2_RENDERER_SWITCH(draw1pMeshes)
// PE 0x10054320 (constructor) / 0x10054410 (converter).
// PE 0x10054410: drawParticles converter.
BF2_RENDERER_SWITCH(drawParticles)
// PE 0x100544C0, 0x100545B0: drawStaticMeshes constructor, converter.
BF2_RENDERER_SWITCH(drawStaticMeshes)
// PE 0x10054670, 0x10054760: drawSkinnedMeshes constructor, converter.
BF2_RENDERER_SWITCH(drawSkinnedMeshes)
// PE 0x10054820 (constructor) / 0x10054910 (converter).
// PE 0x10054910: drawSkyDome converter.
BF2_RENDERER_SWITCH(drawSkyDome)
// PE 0x100549C0 (constructor).
// PE 0x10054B20 (converter: `>> bool` straight into the persistent static, no state test).
// PE 0x10054AB0, 0x10054B20: drawSunFlare return formatter, converter.
BF2_RENDERER_SWITCH(drawSunFlare)
// PE 0x10054BD0, 0x10054CC0: drawRoads constructor, converter.
BF2_RENDERER_SWITCH(drawRoads)
// PE 0x10054D80 (constructor) / 0x10054E70 (converter).
// PE 0x10054E70: drawTerrain converter.
BF2_RENDERER_SWITCH(drawTerrain)
// PE 0x10054F20 (constructor) / 0x10055010 (converter).
// PE 0x10055010: drawUndergrowth converter.
BF2_RENDERER_SWITCH(drawUndergrowth)
// PE 0x100550C0 (constructor) / 0x100551B0 (converter).
// PE 0x100551B0: drawOvergrowth converter.
BF2_RENDERER_SWITCH(drawOvergrowth)
// PE 0x10055260 (constructor).
// PE 0x10055350 (converter: `>> bool` straight into the persistent static, no state test).
// PE 0x10055350: drawNametags converter.
BF2_RENDERER_SWITCH(drawNametags)
// PE 0x10055400 (constructor) / 0x100554F0 (converter).
// PE 0x100554F0: drawTrees converter.
BF2_RENDERER_SWITCH(drawTrees)
// PE 0x100555A0 (constructor) / 0x10055690 (converter).
// PE 0x10055690: drawPostProduction converter.
BF2_RENDERER_SWITCH(drawPostProduction)
// PE 0x10055740 (constructor) / 0x10055830 (converter).
// PE 0x10055830: drawGraphs converter.
BF2_RENDERER_SWITCH(drawGraphs)

// PE 0x100558E0, 0x10055A90, 0x10055B20: makeTopWorldScreen constructor, executor, converter.
BF2_CONSOLE_METHOD_4_STATIC_ACTIVE(makeTopWorldScreen, consoleMakeTopWorldScreen, g_Renderer != 0,
                                   U32, std::string, Vec3, float, kRendererConsoleTag)

// PE 0x10055E60 / 0x10056010.
// PE 0x10055D10, 0x10055E00: nightMode constructor, converter.
// PE 0x100821D0
BF2_CONSOLE_PROPERTY_FN(nightMode, uint, g_Renderer != 0,
                        rendererGetNightMode(), rendererSetNightMode, kRendererConsoleTag)
// PE 0x10055EB0, 0x10055FB0: nightVisionEnabled constructor, converter.
BF2_CONSOLE_PROPERTY_FN(nightVisionEnabled, bool, g_renderTargetSystem != 0,
                        postProductionGetNightVisionEnabled(), postProductionSetNightVisionEnabled,
                        kRendererConsoleTag)

// PE 0x10056070, 0x100561B0: nightVisionGradient constructor, converter.
BF2_RENDERER_WRITEONLY(nightVisionGradient, setNightVisionGradient)
// PE 0x10056230, 0x10056320: nightVisionMaxPower constructor, converter.
BF2_CONSOLE_PROPERTY_FN(nightVisionMaxPower, float, g_Renderer != 0,
                        getNightVisionMaxPower(), setNightVisionMaxPower, kRendererConsoleTag)
// PE 0x10056400, 0x100564F0: nightVisionRegenRPS constructor, converter.
BF2_CONSOLE_PROPERTY_FN(nightVisionRegenRPS, float, g_Renderer != 0,
                        getNightVisionRegenRPS(), setNightVisionRegenRPS, kRendererConsoleTag)
// PE 0x100566F0 / 0x10056900 / 0x10056AA0; writes also reset the timer mark.
// PE 0x100565A0, 0x10056690: fakeHDRBlendingTimeLight constructor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRBlendingTimeLight, double, g_Renderer != 0,
                        rendererTailDouble(0x8C0), rendererSetFakeHDRBlendingTimeLight, kRendererConsoleTag)
// PE 0x10056740, 0x10056830, 0x100568A0: fakeHDRBlendingTimeDark constructor, return formatter, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRBlendingTimeDark, double, g_Renderer != 0,
                        rendererTailDouble(0x8C8), rendererSetFakeHDRBlendingTimeDark, kRendererConsoleTag)
// PE 0x10056950, 0x10056A40: fakeHDRBlendingFactor constructor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRBlendingFactor, float, g_Renderer != 0,
                        rendererTailFloat(0x8BC), rendererSetFakeHDRBlendingFactor, kRendererConsoleTag)
// PE 0x10056B00, 0x10056BF0, 0x10056C70: fakeHDRHorizWeights constructor, executor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRHorizWeights, std::string, g_renderTargetSystem != 0,
                        getFakeHDRHorizWeights(), setFakeHDRHorizWeights, kRendererConsoleTag)
// PE 0x10056CF0, 0x10056DE0, 0x10056E90: fakeHDRVertWeights constructor, executor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRVertWeights, std::string, g_renderTargetSystem != 0,
                        getFakeHDRVertWeights(), setFakeHDRVertWeights, kRendererConsoleTag)
// PE 0x10056F10, 0x10057000, 0x10057050: fakeHDRWeights constructor, executor (one argument:
// clears the has-return flag +0xA0, copies the string, calls setFakeHDRWeights), converter.
BF2_RENDERER_WRITEONLY(fakeHDRWeights, setFakeHDRWeights)
// PE 0x100570D0, 0x100571C0: fakeHDRPassGate constructor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDRPassGate, float, g_renderTargetSystem != 0,
                        getFakeHDRPassGate(), setFakeHDRPassGate, kRendererConsoleTag)
// PE 0x100573E0 / 0x10057570 / 0x10057710 / 0x100578B0 / 0x10057A60.
// PE 0x10057280, 0x10057380: glowEnabled constructor, converter.
BF2_CONSOLE_PROPERTY_FN(glowEnabled, bool, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetGlowEnabled(), postProductionSetConsoleGlowEnabled,
                        kRendererConsoleTag)
// PE 0x10057420, 0x10057510: glowStrength constructor, converter.
BF2_CONSOLE_PROPERTY_FN(glowStrength, float, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetGlowStrength(), postProductionSetGlowStrength,
                        kRendererConsoleTag)
// PE 0x100575C0, 0x100576B0: fakeHDREnabled constructor, converter.
BF2_CONSOLE_PROPERTY_FN(fakeHDREnabled, bool, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetFakeHDREnabled(), postProductionSetFakeHDREnabled,
                        kRendererConsoleTag)
// PE 0x10057760, 0x10057850: lightBlurStrength constructor, converter.
BF2_CONSOLE_PROPERTY_FN(lightBlurStrength, float, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetLightBlurStrength(), postProductionSetLightBlurStrength,
                        kRendererConsoleTag)
// PE 0x10057910, 0x10057A00: tearGasStrength constructor, converter.
BF2_CONSOLE_PROPERTY_FN(tearGasStrength, float, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetTearGasStrength(), postProductionSetTearGasStrength,
                        kRendererConsoleTag)
// PE 0x10057AC0, 0x10057BB0: tvInterference constructor, converter.
BF2_CONSOLE_PROPERTY_FN(tvInterference, float, g_PostProductionFrameWorkOwner != 0,
                        getTVInterference(), setTVInterference,
                        kRendererConsoleTag)
// PE 0x10057C70, 0x10057D60: tvGranularity constructor, converter.
BF2_CONSOLE_PROPERTY_FN(tvGranularity, float, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetTVGranularity(), postProductionSetTVGranularity,
                        kRendererConsoleTag)
// PE 0x10057E10, 0x10057F00: tvAmbient constructor, converter.
BF2_CONSOLE_PROPERTY_FN(tvAmbient, float, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetTVAmbient(), postProductionSetTVAmbient,
                        kRendererConsoleTag)
// PE 0x10057FB0, 0x100580B0, 0x10058120: tvColor constructor, return formatter, converter.
BF2_CONSOLE_PROPERTY_FN(tvColor, Vec3, g_PostProductionFrameWorkOwner != 0,
                        getTVColor(), setTVColor, kRendererConsoleTag)
// PE 0x10058220, 0x10058310: tvEnabled constructor, converter.
BF2_CONSOLE_PROPERTY_FN(tvEnabled, bool, g_PostProductionFrameWorkOwner != 0,
                        postProductionGetTVEnabled(), postProductionSetTVEnabled,
                        kRendererConsoleTag)

// PE 0x100583B0, 0x10058510: ambientIlluminationRange constructor, converter.
BF2_CONSOLE_PROPERTY_FN(ambientIlluminationRange, Vec4, g_PostProductionFrameWorkOwner != 0,
                        getAmbientIlluminationRange(), setAmbientIlluminationRange,
                        kRendererConsoleTag)
// PE 0x10058850 converter / 0x100588D0 executor.
// PE 0x100587B0, 0x100588D0: fogColor return formatter, executor.
BF2_RENDERER_PROPERTY(fogColor, Vec3, : m_returnValue(), g_PostProductionFrameWorkOwner != 0,
                      getFogColor(), setFogColor)
// PE 0x10058A50, 0x10058AD0: waterFogColor converter, executor.
BF2_CONSOLE_PROPERTY_FN(waterFogColor, Vec3, g_PostProductionFrameWorkOwner != 0,
                        getWaterFogColor(), setWaterFogColor, kRendererConsoleTag)
// PE 0x10058C50, 0x10058CC0, 0x10058D50: waterFogStartEndAndBase return formatter, converter, executor.
BF2_CONSOLE_PROPERTY_FN(waterFogStartEndAndBase, Vec4, g_PostProductionFrameWorkOwner != 0,
                        getWaterFogStartEndAndBase(), setWaterFogStartEndAndBase, kRendererConsoleTag)

// [RENDER] renderer.distanceCullConst, distanceCullConstPCO and minCullDistance (PE 0x10058DE0..
// 0x10059210) are deleted: objects no longer cull by distance (milestone 8).
// PE 0x100592D0, 0x100593C0: forceSkinnedMeshLod constructor, converter.
BF2_CONSOLE_PROPERTY_FN(forceSkinnedMeshLod, int, g_Renderer != 0,
                        rendererGetSkinnedMeshLod(), rendererSetSkinnedMeshLod, kRendererConsoleTag)
// PE 0x10059470, 0x10059560: forceBundledMeshLod constructor, converter.
BF2_CONSOLE_PROPERTY_FN(forceBundledMeshLod, int, g_Renderer != 0,
                        rendererGetBundledMeshLod(), rendererSetBundledMeshLod, kRendererConsoleTag)

// PE 0x10059610, 0x10059700: forceStaticMeshSkipLod constructor, converter.
BF2_CONSOLE_PROPERTY_FN(forceStaticMeshSkipLod, int, g_Renderer != 0,
                        getForceStaticMeshSkipLod(), setForceStaticMeshSkipLod, kRendererConsoleTag)
// PE 0x100597B0, 0x100598A0: forceStaticMeshMaxPolyPercentage constructor, converter.
BF2_CONSOLE_PROPERTY_FN(forceStaticMeshMaxPolyPercentage, int, g_Renderer != 0,
                        getForceStaticMeshMaxPolyPercentage(), setForceStaticMeshMaxPolyPercentage,
                        kRendererConsoleTag)
// PE 0x10059950 (constructor) / 0x10059A40 (converter).
// PE 0x10059A40: tweakNearPlane converter.
BF2_CONSOLE_PROPERTY_FN(tweakNearPlane, float, g_Renderer != 0,
                        get_tweakNearPlane(), set_tweakNearPlane, kRendererConsoleTag)
// PE 0x10059AF0 (constructor) / 0x10059BE0 (converter).
// PE 0x10059BE0: tweakNearPlane1p converter.
BF2_CONSOLE_PROPERTY_FN(tweakNearPlane1p, float, g_Renderer != 0,
                        get_tweakNearPlane1p(), set_tweakNearPlane1p, kRendererConsoleTag)
// PE 0x10059C90 (constructor) / 0x10059D80 (converter).
// PE 0x10059D80: tweakFarPlane1p converter.
BF2_CONSOLE_PROPERTY_FN(tweakFarPlane1p, float, g_Renderer != 0,
                        get_tweakFarPlane1p(), set_tweakFarPlane1p, kRendererConsoleTag)
// PE 0x10059E30 (constructor) / 0x10059F20 (converter).
// PE 0x10059F20: coronaFadeSpeed converter.
BF2_CONSOLE_PROPERTY_FN(coronaFadeSpeed, float, g_Renderer != 0,
                        get_coronaFadeSpeed(), set_coronaFadeSpeed, kRendererConsoleTag)
// PE 0x10059FD0, 0x1005A0C0: allowOnDemandPostProcessing constructor, converter.
BF2_CONSOLE_PROPERTY_FN(allowOnDemandPostProcessing, bool, g_Renderer != 0,
                        getAllowOnDemandPostProcessing(), setAllowOnDemandPostProcessing,
                        kRendererConsoleTag)
// PE 0x1005A170 (constructor) / 0x1005A260 (converter).
// PE 0x1005A260: divAccum converter.
BF2_CONSOLE_PROPERTY_FN(divAccum, float, g_Renderer != 0,
                        get_divAccum(), set_divAccum, kRendererConsoleTag)
// PE 0x1005A310, 0x1005A400: ambientSun constructor, converter.
BF2_CONSOLE_PROPERTY_FN(ambientSun, bool, g_Renderer != 0,
                        getAmbientSun(), setAmbientSun, kRendererConsoleTag)

// The three global mesh LOD distance scales (ctors 0x1005A4B0 / 0x1005A650 / 0x1005A7F0, type
// "float"), active while g_Renderer exists (0x1005B930). Every level's Init.con sets them.
// PE 0x1005A5A0: globalStaticMeshLodDistanceScale converter.
BF2_CONSOLE_PROPERTY_FN(globalStaticMeshLodDistanceScale, float, g_Renderer != 0,
                        getGlobalStaticMeshLodDistanceScale(), setGlobalStaticMeshLodDistanceScale,
                        kRendererConsoleTag)
// PE 0x1005A650, 0x1005A740: globalBundleMeshLodDistanceScale constructor, converter.
BF2_CONSOLE_PROPERTY_FN(globalBundleMeshLodDistanceScale, float, g_Renderer != 0,
                        getGlobalBundleMeshLodDistanceScale(), setGlobalBundleMeshLodDistanceScale,
                        kRendererConsoleTag)
// PE 0x1005A7F0, 0x1005A8E0: globalSkinnedMeshLodDistanceScale constructor, converter.
BF2_CONSOLE_PROPERTY_FN(globalSkinnedMeshLodDistanceScale, float, g_Renderer != 0,
                        getGlobalSkinnedMeshLodDistanceScale(), setGlobalSkinnedMeshLodDistanceScale,
                        kRendererConsoleTag)

// PE 0x1005A990, 0x1005AA80: renderAheadLimit constructor, converter.
BF2_CONSOLE_PROPERTY_FN(renderAheadLimit, uint, g_Renderer != 0,
                        getRenderAheadLimit(), setRenderAheadLimit, kRendererConsoleTag)

// [RENDER] Milestone 8: qualityLodEnabled and get/setQualityLodDistance{STM,SKM,BM,ALL} (PE 0x1005AB30
// to 0x1005B940) are gone with the distance shading LODs they drove.
// PE 0x1005BA30, 0x1005BB20
BF2_CONSOLE_PROPERTY_FN(isNightMap, bool, g_Renderer != 0,
                        getIsNightMap(), setIsNightMap, kRendererConsoleTag)

// The water properties, in their constructors' order (0x1005BBE0..0x1005C9A0), just ahead of
// drawHud (0x1005CB70).
// PE 0x1005BBE0 constructor; return formatter 0x1005BCD0 (components through 0x1005BD40).
// PE 0x1005BD70 converter: guarded zeroed static Vec2, readRendererProperty into it, pointer stored.
// PE 0x1005BD70
// PE 0x1005BCD0
BF2_RENDERER_PROPERTY(waterScroll, Vec2, : m_returnValue(), g_Renderer != 0,
                      getWaterScroll(), setWaterScroll)
// PE 0x1005BF40 constructor / 0x1005C030 converter.
// PE 0x1005C030
BF2_RENDERER_PROPERTY(waterAnimSpeed, float, , g_Renderer != 0,
                      getWaterAnimSpeed(), setWaterAnimSpeed)
// PE 0x1005C110 constructor / 0x1005C3C0 converter; executor 0x1005C200.
// PE 0x1005C3C0
BF2_RENDERER_PROPERTY(waterNormalMap, std::string, , g_Renderer != 0,
                      getWaterNormalMap(), setWaterNormalMap)
// PE 0x1005C440 constructor / 0x1005C540 converter.
// PE 0x1005C540
BF2_RENDERER_PROPERTY(waterSpecularColor, Vec4, : m_returnValue(), g_Renderer != 0,
                      getWaterSpecularColor(), setWaterSpecularColor)
// PE 0x1005C710 constructor / 0x1005C810 converter.
// PE 0x1005C810
BF2_RENDERER_PROPERTY(waterColor, Vec3, : m_returnValue(), g_Renderer != 0,
                      getWaterColor(), setWaterColor)
// PE 0x1005C9A0 constructor / 0x1005CA90 converter.
// PE 0x1005CA90
BF2_RENDERER_PROPERTY(waterSpecularPower, float, , g_Renderer != 0,
                      getWaterSpecularPower(), setWaterSpecularPower)

// SetConsoleClassDefaults("renderer", 1, 0) (0x101CE8CE) precedes drawHud: drawHud, drawConsole,
// drawFps and the watermark rows carry access level 1.
BF2_CONSOLE_OBJECT("renderer", 1, 0)

// drawHud - ctor 0x1005CB70 (type "bool"), active while g_Renderer exists (0x1005B930).
// PE 0x1005CC60
BF2_CONSOLE_PROPERTY_FN(drawHud, bool, g_Renderer != 0,
                        rendererGetDrawHud(), rendererSetDrawHud, kRendererConsoleTag)
// PE 0x1005CD10 constructor / 0x1005CE00 converter.
// PE 0x1005CE00
BF2_RENDERER_PROPERTY(drawConsole, bool, , g_Renderer != 0,
                      get_drawConsole(), set_drawConsole)
#undef BF2_RENDERER_PROPERTY
// PE 0x1005CFA0
BF2_CONSOLE_PROPERTY_FN(drawFps, int, g_Renderer != 0,
                        rendererGetDrawFps(), rendererSetDrawFps, kRendererConsoleTag)
// PE 0x1005D0A0, 0x1005D190
BF2_CONSOLE_PROPERTY_FN(drawWatermark, bool, g_Renderer != 0,
                        getDrawWatermark(), setDrawWatermark, kRendererConsoleTag)
// PE 0x1005D330: changing the name does not reload the texture.
// PE 0x1005D240, 0x1005C350, 0x1005D3B0
BF2_CONSOLE_PROPERTY_FN(watermarkFilename, std::string, g_Renderer != 0,
                        rendererGetWatermarkFilename(), rendererSetWatermarkFilename, kRendererConsoleTag)
// PE 0x1005D430, 0x1005D520
BF2_CONSOLE_PROPERTY_FN(watermarkScale, float, g_Renderer != 0,
                        getWatermarkScale(), setWatermarkScale, kRendererConsoleTag)
// PE 0x1005D5D0, 0x1005D6C0, 0x1005D750
BF2_CONSOLE_PROPERTY_FN(watermarkPos, Vec2, g_Renderer != 0,
                        getWatermarkPos(), setWatermarkPos, kRendererConsoleTag)

// shadermanager - SetConsoleClassDefaults("shadermanager", 2, 0) between drawFps and texturemanager
// in the shipped initialiser. Every binding is active while the shader manager exists (0x1005E850).
// reloadAllEffects (ctor 0x1005D860, executor 0x1005D8F0 -> 0x10023540), reloadEffect (0x1005D910,
// executor 0x1005DA10 -> 0x10023690), listEffects (0x1005DAE0, executor 0x1005DB70 -> 0x10022A80),
// then the eight bool switches (ctors 0x1005DB90 .. 0x1005E760; ShaderManager.cpp lists the arms).
void consoleReloadAllEffects() { shaderManagerReloadAllEffects(g_shaderManager); }

bool getCompileUsePS2A()                  { return shaderManagerGetConsoleSwitch(0); }
void setCompileUsePS2A(bool v)            { shaderManagerSetConsoleSwitch(0, v); }
bool getCompilePartialPrecisionTypes()    { return shaderManagerGetConsoleSwitch(1); }
void setCompilePartialPrecisionTypes(bool v) { shaderManagerSetConsoleSwitch(1, v); }
bool getCompileUsePartialPrecision()      { return shaderManagerGetConsoleSwitch(2); }
void setCompileUsePartialPrecision(bool v) { shaderManagerSetConsoleSwitch(2, v); }
bool getCompileUsePreShaders()            { return shaderManagerGetConsoleSwitch(3); }
void setCompileUsePreShaders(bool v)      { shaderManagerSetConsoleSwitch(3, v); }
bool getCompileSkipOptimizations()        { return shaderManagerGetConsoleSwitch(4); }
void setCompileSkipOptimizations(bool v)  { shaderManagerSetConsoleSwitch(4, v); }
bool getCompileWithDebug()                { return shaderManagerGetConsoleSwitch(5); }
void setCompileWithDebug(bool v)          { shaderManagerSetConsoleSwitch(5, v); }
bool getCompileDebugTerrain()             { return shaderManagerGetConsoleSwitch(6); }
void setCompileDebugTerrain(bool v)       { shaderManagerSetConsoleSwitch(6, v); }
bool getByPassDiceStates()                { return shaderManagerGetConsoleSwitch(7); }
void setByPassDiceStates(bool v)          { shaderManagerSetConsoleSwitch(7, v); }

BF2_CONSOLE_OBJECT("shadermanager", 2, 0)
// PE 0x1005D860
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(reloadAllEffects, consoleReloadAllEffects, g_shaderManager != 0,
                                   kRendererConsoleTag)
// PE 0x1005D910, 0x1005DA60
BF2_CONSOLE_METHOD_1_STATIC_ACTIVE(reloadEffect, shaderManagerConsoleReloadEffect,
                                   g_shaderManager != 0, std::string, kRendererConsoleTag)
// PE 0x1005DAE0
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(listEffects, shaderManagerListEffects, g_shaderManager != 0,
                                   kRendererConsoleTag)
// PE 0x1005DB90, 0x1005DCE0, 0x1005DC80
BF2_CONSOLE_PROPERTY_FN(compileUsePS2A, bool, g_shaderManager != 0,
                        getCompileUsePS2A(), setCompileUsePS2A, kRendererConsoleTag)
// PE 0x1005DD40, 0x1005DE90, 0x1005DE30
BF2_CONSOLE_PROPERTY_FN(compilePartialPrecisionTypes, bool, g_shaderManager != 0,
                        getCompilePartialPrecisionTypes(), setCompilePartialPrecisionTypes,
                        kRendererConsoleTag)
// PE 0x1005DEF0, 0x1005E040, 0x1005DFE0
BF2_CONSOLE_PROPERTY_FN(compileUsePartialPrecision, bool, g_shaderManager != 0,
                        getCompileUsePartialPrecision(), setCompileUsePartialPrecision,
                        kRendererConsoleTag)
// PE 0x1005E0A0, 0x1005E1F0, 0x1005E190
BF2_CONSOLE_PROPERTY_FN(compileUsePreShaders, bool, g_shaderManager != 0,
                        getCompileUsePreShaders(), setCompileUsePreShaders, kRendererConsoleTag)
// PE 0x1005E250, 0x1005E3A0, 0x1005E340
BF2_CONSOLE_PROPERTY_FN(compileSkipOptimizations, bool, g_shaderManager != 0,
                        getCompileSkipOptimizations(), setCompileSkipOptimizations,
                        kRendererConsoleTag)
// PE 0x1005E400, 0x1005E550, 0x1005E4F0
BF2_CONSOLE_PROPERTY_FN(compileWithDebug, bool, g_shaderManager != 0,
                        getCompileWithDebug(), setCompileWithDebug, kRendererConsoleTag)
// PE 0x1005E5B0, 0x1005E700, 0x1005E6A0
BF2_CONSOLE_PROPERTY_FN(compileDebugTerrain, bool, g_shaderManager != 0,
                        getCompileDebugTerrain(), setCompileDebugTerrain, kRendererConsoleTag)
// PE 0x1005E760, 0x1005E860
BF2_CONSOLE_PROPERTY_FN(byPassDiceStates, bool, g_shaderManager != 0,
                        getByPassDiceStates(), setByPassDiceStates, kRendererConsoleTag)

// texturemanager.customTextureSuffix - ctor 0x1005E910 (type "std::string", dynamic), active
// while the texture manager exists (0x1005EA00). Level Init.con files set it to pick their
// camouflage texture variant ("Woodland" on Dalian Plant, "" on Operation Clean Sweep).
BF2_CONSOLE_OBJECT("texturemanager", 2, 0)
// PE 0x1005EA90
BF2_CONSOLE_PROPERTY_FN(customTextureSuffix, std::string, g_textureManager != 0,
                        textureManagerGetCustomTextureSuffix(), textureManagerSetCustomTextureSuffix,
                        kRendererConsoleTag)

BF2_CONSOLE_OBJECT("Renderer", 2, 0)

// PE 0x100B1E27 (metadata), 0x100B1EE5 (executor), 0x100B4F7C (always active), 0x100B1E98 (argument
// parser: one float into persistent static storage; the executor never reads it).
class RendererFieldOfViewBinding : public ConsoleObjectBaseImpl
{
public:
    // PE 0x100B1E27
    RendererFieldOfViewBinding()
    {
        m_objectName = consoleDefaultStaticName();   // "Renderer" as passed (dword_10247C20)
        m_access = consoleDefaultAccess();
        m_category = consoleDefaultCategory();
        m_functionName = "fieldOfView1p";
        m_type = 1;
        m_minNoArgs = 0;
        m_maxNoArgs = 1;
        m_returnType = "float";
        m_hasReturnValue = false;
        m_isDynamic = false;
        m_argTypes[0] = "float";
        m_argNeedsConversion[0] = -1;
        consoleBindingList(kRendererConsoleTag).push_back(this);
    }
    virtual bool isObjectActive() const { return true; } // PE 0x100B4F7C.
    virtual void executeObjectMethod()
    {
        if (m_argCount == 0)
        {
            m_hasReturnValue = true;
            m_returnValue = 1.2f; // PE dword_10238FEC: the executor is its only reference.
        }
        else if (m_argCount == 1)
        {
            // shipped defect: consume the command without reading the parsed value or storing it.
            m_hasReturnValue = false;
        }
    }
    // PE 0x100B1E98
    virtual void setArgFromString(int index, const std::string& value)
    {
        BF2_CC_CONVERT(float, 1)
    }
    BF2_CC_RETURN_AS_STRING
    BF2_CC_COMMON_HOOKS
private:
    float m_returnValue;
};
static RendererFieldOfViewBinding s_fieldOfView1p;

// PE 0x100B0DAB. The activity check and each executor perform their own lookup.
IBase* consoleRoadCompiledRenderer()
{
    const renddx9abi::HostString className("RoadCompiledRenderer");
    return classManager->getSingleton(className);
}

bool getDisableFadeOnRoads()
{
    return geom::roadCompiledRendererConsoleDisableFade(consoleRoadCompiledRenderer()) != 0;
}
void setDisableFadeOnRoads(bool value)
{
    geom::roadCompiledRendererConsoleDisableFade(consoleRoadCompiledRenderer()) = value;
}
float getRoadDepthBias()
{
    return geom::roadCompiledRendererConsoleDepthBias(consoleRoadCompiledRenderer());
}
void setRoadDepthBias(float value)
{
    geom::roadCompiledRendererConsoleDepthBias(consoleRoadCompiledRenderer()) = value;
}
float getRoadSlopeScaleDepthBias()
{
    return geom::roadCompiledRendererConsoleSlopeScaleDepthBias(consoleRoadCompiledRenderer());
}
void setRoadSlopeScaleDepthBias(float value)
{
    geom::roadCompiledRendererConsoleSlopeScaleDepthBias(consoleRoadCompiledRenderer()) = value;
}
float getRoadLodDistanceModifier()
{
    return geom::roadCompiledRendererConsoleLodDistanceModifier(consoleRoadCompiledRenderer());
}
void setRoadLodDistanceModifier(float value)
{
    geom::roadCompiledRendererConsoleLodDistanceModifier(consoleRoadCompiledRenderer()) = value;
}

// PE 0x100B1FDB / 0x100B20E3 / 0x100B21E9 / 0x100B22EF.
// PE 0x100B1F11, 0x100B1F8E
BF2_CONSOLE_PROPERTY_FN(disableFadeOnRoads, bool, consoleRoadCompiledRenderer() != 0,
                        getDisableFadeOnRoads(), setDisableFadeOnRoads, kRendererConsoleTag)
// PE 0x100B2025, 0x100B6B6B, 0x100B2096
BF2_CONSOLE_PROPERTY_FN(roadDepthBias, float, consoleRoadCompiledRenderer() != 0,
                        getRoadDepthBias(), setRoadDepthBias, kRendererConsoleTag)
// PE 0x100B212B, 0x100B219C
BF2_CONSOLE_PROPERTY_FN(roadSlopeScaleDepthBias, float, consoleRoadCompiledRenderer() != 0,
                        getRoadSlopeScaleDepthBias(), setRoadSlopeScaleDepthBias, kRendererConsoleTag)
// PE 0x100B2231, 0x100B22A2
BF2_CONSOLE_PROPERTY_FN(roadLodDistMod, float, consoleRoadCompiledRenderer() != 0,
                        getRoadLodDistanceModifier(), setRoadLodDistanceModifier, kRendererConsoleTag)

// [UI] The Win32 virtual-key code that opens and closes the fork's menu (docs/lanes/ui.md);
// 45 (Insert) by default.
BF2_CONSOLE_PROPERTY_FN(uiMenuKey, int, true, uiLayerGetMenuKey(), uiLayerSetMenuKey,
                        kRendererConsoleTag)
// [UI] The console window's share of the screen height (docs/lanes/ui.md); saved like uiOverlay.
BF2_CONSOLE_PROPERTY_FN_RANGED_T(uiConsoleHeight, float, true, uiLayerGetConsoleHeight(),
                                 uiLayerSetConsoleHeight, 0.2f, 0.9f, kRendererConsoleTag)
// [UI] The performance overlay, top right (docs/lanes/ui.md); kept between sessions in the mod's
// settings/eon64-ui.con.
BF2_CONSOLE_PROPERTY_FN(uiOverlay, bool, true, uiLayerGetOverlay(), uiLayerSetOverlay,
                        kRendererConsoleTag)

// [RENDER] Sun shadow tuning (docs/render/shadows.md, sun shadows). Each variable clamps to its
// range; the defaults are the values the cascades were accepted with.
#define BF2_SUN_SHADOW_VARIABLE(name, T, field, minValue, maxValue)                                 \
    T get_##name() { return static_cast<T>(sunShadowSettings().field); }                            \
    void set_##name(T value)                                                                        \
    {                                                                                               \
        sunShadowSettings().field = value < (minValue) ? (minValue)                                 \
                                  : value > (maxValue) ? (maxValue) : value;                        \
    }                                                                                               \
    BF2_CONSOLE_PROPERTY_FN_RANGED_T(name, T, true, get_##name(), set_##name, minValue, maxValue,   \
                                     kRendererConsoleTag)
BF2_SUN_SHADOW_VARIABLE(sunShadowDistance, float, distance, 50.0f, 2000.0f)
BF2_SUN_SHADOW_VARIABLE(sunShadowCasterReach, float, casterReach, 0.0f, 2000.0f)
BF2_SUN_SHADOW_VARIABLE(sunShadowSplitLambda, float, splitLambda, 0.0f, 1.0f)
BF2_SUN_SHADOW_VARIABLE(sunShadowCascadeBlend, float, cascadeBlend, 0.0f, 0.5f)
BF2_SUN_SHADOW_VARIABLE(sunShadowNormalOffset, float, normalOffset, 0.0f, 4.0f)
BF2_SUN_SHADOW_VARIABLE(sunShadowDepthBias, float, depthBias, 0.0f, 4.0f)
#undef BF2_SUN_SHADOW_VARIABLE

bool getSunShadows() { return sunShadowSettings().enabled; }
void setSunShadows(bool value) { sunShadowSettings().enabled = value; }
BF2_CONSOLE_PROPERTY_FN(sunShadows, bool, true, getSunShadows(), setSunShadows, kRendererConsoleTag)

// [RENDER] Render milestone 14: the static cascade's receiver offsets, in its texels.
float getSunStaticNormalOffset() { return sunStaticSettings().normalOffset; }
void setSunStaticNormalOffset(float value) { sunStaticSettings().normalOffset = value < 0.0f ? 0.0f : value > 8.0f ? 8.0f : value; }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(sunStaticNormalOffset, float, true, getSunStaticNormalOffset(),
                                 setSunStaticNormalOffset, 0.0f, 8.0f, kRendererConsoleTag)
float getSunStaticDepthBias() { return sunStaticSettings().depthBias; }
void setSunStaticDepthBias(float value) { sunStaticSettings().depthBias = value < 0.0f ? 0.0f : value > 8.0f ? 8.0f : value; }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(sunStaticDepthBias, float, true, getSunStaticDepthBias(),
                                 setSunStaticDepthBias, 0.0f, 8.0f, kRendererConsoleTag)
// Texels per side of the static cascade: a power of two in 1024..8192 (redrawn on change).
int getSunStaticResolution() { return static_cast<int>(sunStaticSettings().resolution); }
void setSunStaticResolution(int value)
{
    unsigned int size = 1024;
    while (size < 8192 && value > static_cast<int>(size + size / 2))
        size *= 2;
    sunStaticSettings().resolution = size;
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(sunStaticResolution, int, true, getSunStaticResolution(),
                                 setSunStaticResolution, 1024, 8192, kRendererConsoleTag)

// [RENDER] Contribution culling (milestone 9): a static mesh whose bounding sphere projects to fewer
// pixels across than this is not drawn, and fades out over the 20 % above it; 0 turns it off.
float getContributionCull() { return rendererContributionCullPixels(); }
void setContributionCull(float value)
{
    rendererSetContributionCullPixels(value < 0.0f ? 0.0f : value > 32.0f ? 32.0f : value);
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(contributionCull, float, true, getContributionCull(),
                                 setContributionCull, 0.0f, 32.0f, kRendererConsoleTag)

// Texels per cascade side: the nearest power of two in 512..4096.
int getSunShadowResolution() { return static_cast<int>(sunShadowSettings().resolution); }
void setSunShadowResolution(int value)
{
    unsigned int size = 512;
    while (size < 4096 && value > static_cast<int>(size + size / 2))
        size *= 2;
    sunShadowSettings().resolution = size;
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(sunShadowResolution, int, true, getSunShadowResolution(),
                                 setSunShadowResolution, 512, 4096, kRendererConsoleTag)

// [RENDER] Soft particles (docs/render/particles.md): the depth over which a particle fades out in
// front of the scene behind it, in metres.
float getSoftParticleDistance() { return softParticleDistance(); }
void setSoftParticleDistance(float value)
{
    softParticleDistance() = value < 0.1f ? 0.1f : value > 20.0f ? 20.0f : value;
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(softParticleDistance, float, true, getSoftParticleDistance(),
                                 setSoftParticleDistance, 0.1f, 20.0f, kRendererConsoleTag)

// [RENDER] The weather's air (render milestone 18): the atmosphere rebuilds its tables on a worker
// when one changes.
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float getWeatherVisibility() { return rendererAtmosphereWeather().visibility; }
void setWeatherVisibility(float value) { rendererAtmosphereWeather().visibility = clampf(value, 1000.0f, 200000.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(weatherVisibility, float, true, getWeatherVisibility(), setWeatherVisibility,
                                 1000.0f, 200000.0f, kRendererConsoleTag)
int getWeatherAerosol() { return rendererAtmosphereWeather().aerosol; }
void setWeatherAerosol(int value) { rendererAtmosphereWeather().aerosol = value < 0 ? 0 : value > 3 ? 3 : value; }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(weatherAerosol, int, true, getWeatherAerosol(), setWeatherAerosol, 0, 3,
                                 kRendererConsoleTag)
float getWeatherMixing() { return rendererAtmosphereWeather().mixing; }
void setWeatherMixing(float value) { rendererAtmosphereWeather().mixing = clampf(value, 100.0f, 5000.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(weatherMixing, float, true, getWeatherMixing(), setWeatherMixing, 100.0f, 5000.0f,
                                 kRendererConsoleTag)
float getWeatherAltitude() { return rendererAtmosphereWeather().altitude; }
void setWeatherAltitude(float value)
{
    rendererAtmosphereWeather().altitude = value < -1000.0f ? kAltitudeAuto : clampf(value, -500.0f, 6000.0f);
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(weatherAltitude, float, true, getWeatherAltitude(), setWeatherAltitude,
                                 kAltitudeAuto, 6000.0f, kRendererConsoleTag)

// [RENDER] The sky clouds (render milestone 12): drawn or not, how many, and which climate's.
bool getClouds() { return rendererCloudsEnabled(); }
void setClouds(bool value) { rendererCloudsEnabled() = value; }
BF2_CONSOLE_PROPERTY_FN(clouds, bool, true, getClouds(), setClouds, kRendererConsoleTag)
float getCloudCoverage() { return rendererCloudCoverage(); }
void setCloudCoverage(float value)
{
    rendererCloudCoverage() = clampf(value, 0.0f, 1.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudCoverage, float, true, getCloudCoverage(), setCloudCoverage, 0.0f, 1.0f,
                                 kRendererConsoleTag)
int getCloudPreset() { return rendererCloudPreset(); }
void setCloudPreset(int value)
{
    rendererCloudPreset() = value < 0 ? 0 : value > 1 ? 1 : value;
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudPreset, int, true, getCloudPreset(), setCloudPreset, 0, 1,
                                 kRendererConsoleTag)
float getCloudBase() { return rendererCloudBase(); }
void setCloudBase(float value)
{
    rendererCloudBase() = clampf(value, 0.0f, 12000.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudBase, float, true, getCloudBase(), setCloudBase, 0.0f, 12000.0f,
                                 kRendererConsoleTag)
float getCloudTop() { return rendererCloudTop(); }
void setCloudTop(float value)
{
    rendererCloudTop() = clampf(value, 0.0f, 12000.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudTop, float, true, getCloudTop(), setCloudTop, 0.0f, 12000.0f,
                                 kRendererConsoleTag)
float getCloudMidBase() { return rendererCloudMidBase(); }
void setCloudMidBase(float value)
{
    rendererCloudMidBase() = clampf(value, 0.0f, 12000.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudMidBase, float, true, getCloudMidBase(), setCloudMidBase, 0.0f, 12000.0f,
                                 kRendererConsoleTag)
float getCloudMidTop() { return rendererCloudMidTop(); }
void setCloudMidTop(float value)
{
    rendererCloudMidTop() = clampf(value, 0.0f, 12000.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudMidTop, float, true, getCloudMidTop(), setCloudMidTop, 0.0f, 12000.0f,
                                 kRendererConsoleTag)
float getCloudMidDensity() { return rendererCloudMidDensity(); }
void setCloudMidDensity(float value)
{
    rendererCloudMidDensity() = clampf(value, -1.0f, 2.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudMidDensity, float, true, getCloudMidDensity(), setCloudMidDensity, -1.0f, 2.0f,
                                 kRendererConsoleTag)
float getCloudMidCoverage() { return rendererCloudMidCoverage(); }
void setCloudMidCoverage(float value)
{
    rendererCloudMidCoverage() = clampf(value, -1.0f, 1.0f);
    rendererCloudSettingsChanged();
}
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudMidCoverage, float, true, getCloudMidCoverage(), setCloudMidCoverage, -1.0f, 1.0f,
                                 kRendererConsoleTag)
float getCloudNear() { return rendererCloudNearDistance(); }
void setCloudNear(float value) { rendererCloudNearDistance() = clampf(value, 0.0f, 40000.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(cloudNear, float, true, getCloudNear(), setCloudNear, 0.0f, 40000.0f,
                                 kRendererConsoleTag)

// [RENDER] Water's waves (render milestone 16): the FFT ocean's sea.
float getWaterWind() { return rendererWaterWind(); }
void setWaterWind(float value) { rendererWaterWind() = clampf(value, 1.0f, 30.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(waterWind, float, true, getWaterWind(), setWaterWind, 1.0f, 30.0f,
                                 kRendererConsoleTag)
float getWaterFetch() { return rendererWaterFetch(); }
void setWaterFetch(float value) { rendererWaterFetch() = clampf(value, 1.0f, 1000.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(waterFetch, float, true, getWaterFetch(), setWaterFetch, 1.0f, 1000.0f,
                                 kRendererConsoleTag)
// [RENDER] render milestone 23: the bearing the wind blows from; -1 from the open-water map's most open sea.
float getWaterWindFrom() { return rendererWaterWindFrom(); }
void setWaterWindFrom(float value) { rendererWaterWindFrom() = value < 0.0f ? -1.0f : std::fmod(value, 360.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(waterWindFrom, float, true, getWaterWindFrom(), setWaterWindFrom, -1.0f, 360.0f,
                                 kRendererConsoleTag)
float getWaterFoam() { return rendererWaterFoam(); }
void setWaterFoam(float value) { rendererWaterFoam() = clampf(value, 0.0f, 4.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(waterFoam, float, true, getWaterFoam(), setWaterFoam, 0.0f, 4.0f,
                                 kRendererConsoleTag)
// [RENDER] render milestone 23 task 2's A/B switch, until acceptance: the open-water map bound.
bool getWaterOpen() { return rendererWaterOpen(); }
void setWaterOpen(bool value) { rendererWaterOpen() = value; }
BF2_CONSOLE_PROPERTY_FN(waterOpen, bool, true, getWaterOpen(), setWaterOpen, kRendererConsoleTag)

// [RENDER] The far ground (render milestone 17, Geom/FarGround.cpp): 0 the retail ring, 1 where the
// level has a bake, 2 everywhere.
int getFarGround() { return rendererFarGround(); }
void setFarGround(int value) { rendererFarGround() = value < 0 ? 0 : (value > 2 ? 2 : value); }
BF2_CONSOLE_PROPERTY_FN(farGround, int, true, getFarGround(), setFarGround, kRendererConsoleTag)
bool getFarGroundScaled() { return rendererFarGroundScaled(); }
void setFarGroundScaled(bool value) { rendererFarGroundScaled() = value; }
BF2_CONSOLE_PROPERTY_FN(farGroundScaled, bool, true, getFarGroundScaled(), setFarGroundScaled, kRendererConsoleTag)

// [RENDER] Hex-tiled terrain detail (render milestone 19, TerrainDraw.cpp).
bool getHexTiling() { return rendererHexTiling(); }
void setHexTiling(bool value) { rendererHexTiling() = value; }
BF2_CONSOLE_PROPERTY_FN(hexTiling, bool, true, getHexTiling(), setHexTiling, kRendererConsoleTag)

// [RENDER] Bloom and auto-exposure (render milestone 13, HdrPost.cpp).
float getBloomStrength() { return rendererBloomStrength(); }
void setBloomStrength(float value) { rendererBloomStrength() = clampf(value, 0.0f, 0.5f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(bloomStrength, float, true, getBloomStrength(), setBloomStrength, 0.0f, 0.5f,
                                 kRendererConsoleTag)
float getExposureBias() { return rendererExposureBias(); }
void setExposureBias(float value) { rendererExposureBias() = clampf(value, -8.0f, 8.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(exposureBias, float, true, getExposureBias(), setExposureBias, -8.0f, 8.0f,
                                 kRendererConsoleTag)
float getExposureMin() { return rendererExposureMin(); }
void setExposureMin(float value) { rendererExposureMin() = clampf(value, -12.0f, 12.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(exposureMin, float, true, getExposureMin(), setExposureMin, -12.0f, 12.0f,
                                 kRendererConsoleTag)
float getExposureMax() { return rendererExposureMax(); }
void setExposureMax(float value) { rendererExposureMax() = clampf(value, -12.0f, 12.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(exposureMax, float, true, getExposureMax(), setExposureMax, -12.0f, 12.0f,
                                 kRendererConsoleTag)
float getExposureSpeed() { return rendererExposureSpeed(); }
void setExposureSpeed(float value) { rendererExposureSpeed() = clampf(value, 0.05f, 20.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(exposureSpeed, float, true, getExposureSpeed(), setExposureSpeed, 0.05f, 20.0f,
                                 kRendererConsoleTag)
float getExposureStrength() { return rendererExposureStrength(); }
void setExposureStrength(float value) { rendererExposureStrength() = clampf(value, 0.05f, 1.0f); }
BF2_CONSOLE_PROPERTY_FN_RANGED_T(exposureStrength, float, true, getExposureStrength(), setExposureStrength, 0.05f,
                                 1.0f, kRendererConsoleTag)

// [RENDER] The frame grade's look (render milestone 21, Grade.cpp).
int getGradeCurve() { return rendererGrade().curve; }
void setGradeCurve(int value) { rendererGrade().curve = value < 0 ? 0 : value > 1 ? 1 : value; }
BF2_CONSOLE_PROPERTY_FN(gradeCurve, int, true, getGradeCurve(), setGradeCurve, kRendererConsoleTag)
#define GRADE_FLOAT(NAME, FIELD, LO, HI) \
    float get##NAME() { return rendererGrade().FIELD; } \
    void set##NAME(float value) { rendererGrade().FIELD = clampf(value, LO, HI); } \
    BF2_CONSOLE_PROPERTY_FN_RANGED_T(NAME, float, true, get##NAME(), set##NAME, LO, HI, kRendererConsoleTag)
GRADE_FLOAT(gradeContrast, contrast, 0.5f, 2.0f)
GRADE_FLOAT(gradeSaturation, saturation, 0.0f, 2.0f)
GRADE_FLOAT(gradeHighlightShift, highlightShift, 0.0f, 1.0f)
GRADE_FLOAT(gradeToe, toe, 1.0f, 2.0f)
GRADE_FLOAT(gradeShoulder, shoulder, 0.05f, 1.0f)
GRADE_FLOAT(gradeTemperature, temperature, 2000.0f, 15000.0f)
GRADE_FLOAT(gradeTint, tint, -1.0f, 1.0f)
GRADE_FLOAT(gradeShadowSaturation, shadowSaturation, 0.0f, 2.0f)
GRADE_FLOAT(gradeHighlightSaturation, highlightSaturation, 0.0f, 2.0f)
GRADE_FLOAT(gradeNight, night, 0.0f, 1.0f)
#undef GRADE_FLOAT
#define GRADE_TINT(NAME, FIELD) \
    Vec3 get##NAME() { const float* t = rendererGrade().FIELD; Vec3 v; v.x = t[0]; v.y = t[1]; v.z = t[2]; return v; } \
    void set##NAME(const Vec3& v) \
    { \
        float* t = rendererGrade().FIELD; \
        t[0] = clampf(v.x, 0.0f, 4.0f); t[1] = clampf(v.y, 0.0f, 4.0f); t[2] = clampf(v.z, 0.0f, 4.0f); \
    } \
    BF2_CONSOLE_PROPERTY_FN(NAME, Vec3, true, get##NAME(), set##NAME, kRendererConsoleTag)
GRADE_TINT(gradeShadowTint, shadowTint)
GRADE_TINT(gradeMidTint, midTint)
GRADE_TINT(gradeHighlightTint, highlightTint)
#undef GRADE_TINT

} // anonymous namespace

// The help lines of the fork's renderer variables, for the debug menu (docs/lanes/ui.md).
const char* rendererVariableHelp(const char* name)
{
    static const char* const kHelp[][2] = {
        { "sunShadows", "Sun shadow cascades; off, surfaces take the baked lightmap sun" },
        { "sunShadowDistance", "View depth the sun shadow cascades reach, in metres" },
        { "sunShadowResolution", "Texels per cascade side (512-4096, a power of two); the atlas holds four" },
        { "sunShadowCasterReach", "How far toward the sun beyond a cascade casters still draw, in metres" },
        { "sunShadowSplitLambda", "Cascade splits: 0 uniform, 1 logarithmic (finer near the camera)" },
        { "sunShadowCascadeBlend", "Part of the next slice each cascade also covers and blends over" },
        { "sunShadowNormalOffset", "Receiver offset along its normal, in cascade texels (against acne)" },
        { "sunShadowDepthBias", "Receiver offset toward the sun, in cascade texels (against acne)" },
        { "sunStaticResolution", "Static sun cascade over the map: texels per side (1024-8192), drawn once per sun" },
        { "sunStaticNormalOffset", "Static sun cascade: receiver offset along its normal, in its texels" },
        { "sunStaticDepthBias", "Static sun cascade: receiver offset toward the sun, in its texels, times 1 + the slope's tangent" },
        { "contributionCull", "Static meshes whose bounding sphere spans fewer pixels than this are not drawn, faded out just above it; 0 off" },
        { "weatherVisibility", "Weather: metres at which a level ray at the level's ground keeps 2 % contrast (lower: hazier)" },
        { "weatherAerosol", "Weather: the haze's kind, 0 continental (grey), 1 maritime (white), 2 dust (yellow-brown), 3 smoke" },
        { "weatherMixing", "Weather: how high over the ground the haze reaches, metres; it thins above" },
        { "weatherAltitude", "Weather: the level's ground above sea level, metres; below -1000 from its far-ground bake, else sea level" },
        { "clouds", "Sky clouds; off, the clear sky (and no cloud volumes are built)" },
        { "cloudCoverage", "Sky clouds: how much of the sky they cover, 0 clear to 1 overcast" },
        { "waterWind", "Water's waves: the wind speed, m/s (1-30)" },
        { "waterFetch", "Water's waves: the fetch, how far the wind has blown over water, km (1-1000)" },
        { "waterWindFrom", "Water's waves: the bearing the wind blows from, degrees (0 from north, +z; 90 from east, +x); -1 from the map's most open sea" },
        { "waterOpen", "Water's open-water map (milestone 23 A/B): waves and foam by the local fetch upwind; off, the level's sea everywhere" },
        { "waterFoam", "Water's foam on the crests: times the whitecap coverage measured for the wind (0-4); 0 none" },
        { "cloudPreset", "Sky clouds' climate: 0 desert (high cumulus), 1 maritime (low stratocumulus)" },
        { "cloudBase", "Sky clouds' low layer: base above the terrain's mean height, in metres; 0 the preset's (desert 2000, maritime 500)" },
        { "cloudTop", "Sky clouds' low layer: top above the terrain's mean height, in metres (at least base + 500); 0 the preset's (desert 6000, maritime 4500)" },
        { "cloudMidBase", "Sky clouds' mid layer: base above the terrain's mean height, in metres; 0 the preset's (desert 6500, maritime 3800)" },
        { "cloudMidTop", "Sky clouds' mid layer: top, in metres (at least base + 500); 0 the preset's (desert 8000, maritime 5500)" },
        { "cloudMidDensity", "Sky clouds' mid layer: density against the low layer's; -1 the preset's (desert 0.6, maritime 0.35)" },
        { "cloudMidCoverage", "Sky clouds' mid layer: coverage, 0 clear to 1 overcast; -1 the preset's (cloudCoverage times 0.8 desert, 0.6 maritime)" },
        { "cloudNear", "Sky clouds marched per pixel (in front of the scene) out to this many metres; the panorama beyond; 0 panorama only" },
        { "farGround", "Far ground past the map, to about 32 km: 0 the surrounding ring, 1 on levels with a baked terrain file (the ring elsewhere), 2 on every level" },
        { "farGroundScaled", "Far ground's baked real terrain: its heights scaled to the map's relief (about the sea, or the map's median height); off, real heights" },
        { "hexTiling", "Detail textures hex-tiled (each hexagon shifted at random), terrain and the static meshes the list names: no repeating grid; off, tiled on a square grid as retail" },
        { "bloomStrength", "Bloom (the scene's light scattered in the lens, replacing retail's glow): its share of the scene (0-0.5)" },
        { "exposureBias", "Exposure compensation in stops, over the auto-exposure" },
        { "exposureMin", "Auto-exposure: the lowest exposure, in stops (how far bright scenes are darkened)" },
        { "exposureMax", "Auto-exposure: the highest exposure, in stops (keeps night dark)" },
        { "exposureSpeed", "Auto-exposure: adaptation speed, times 3/s into light and 1/s into dark" },
        { "exposureStrength", "Auto-exposure: share of a darkening it lifts back (1 all: interiors as bright as outside; lower keeps shade and rooms darker); bright scenes are darkened in full" },
        { "gradeCurve", "Tonemap curve: 1 the GT curve (toe, linear mids, shoulder; highlights may shift toward white), 0 PBR Neutral with the old fixed grade (A/B)" },
        { "gradeContrast", "Grade: contrast around mid grey, before the curve (1 none)" },
        { "gradeSaturation", "Grade: saturation, before the curve (1 none)" },
        { "gradeHighlightShift", "Grade: bright colours toward white, channel by channel (fire goes yellow, then white); 0 keeps their hue" },
        { "gradeToe", "Grade: the curve's toe below mid tones (1 linear; higher, deeper shadows)" },
        { "gradeShoulder", "Grade: length of the curve's straight section before the highlights roll off (higher, later and harder roll-off)" },
        { "gradeTemperature", "Grade: white balance, the light the image is balanced for in kelvin (6504 neutral; lower cooler, higher warmer)" },
        { "gradeTint", "Grade: white balance tint (+ magenta, - green)" },
        { "gradeShadowTint", "Grade: colour of the shadows (below mid grey), r/g/b multiplier with its brightness divided out; 1/1/1 none" },
        { "gradeMidTint", "Grade: colour of the mid tones, r/g/b multiplier with its brightness divided out; 1/1/1 none" },
        { "gradeHighlightTint", "Grade: colour of the highlights (above mid grey), r/g/b multiplier with its brightness divided out; 1/1/1 none" },
        { "gradeShadowSaturation", "Grade: saturation of the shadows, times gradeSaturation (1 none)" },
        { "gradeHighlightSaturation", "Grade: saturation of the highlights, times gradeSaturation (1 none)" },
        { "gradeNight", "Grade: low light, when the scene is darker than the auto-exposure can lift: dark areas shift blue and desaturate by this much (0 none); the toe relaxes either way" },
        { "softParticleDistance", "Depth over which particles fade out in front of the scene, in metres" },
        { "uiMenuKey", "Win32 virtual-key code that opens and closes this menu (45 = Insert)" },
        { "uiOverlay", "Performance overlay, top right; kept between sessions" },
        { "uiConsoleHeight", "Console window height, as a share of the screen; kept between sessions" },
    };
    for (size_t i = 0; i < sizeof(kHelp) / sizeof(kHelp[0]); ++i)
        if (std::strcmp(kHelp[i][0], name) == 0)
            return kHelp[i][1];
    return 0;
}

bool registerRendererConsoleObjects()
{
    return mainConsoleObjects->registerConsoleObjects(
        consoleBindingList(kRendererConsoleTag));
}

bool unregisterRendererConsoleObjects()
{
    return mainConsoleObjects->unregisterConsoleObjects(
        consoleBindingList(kRendererConsoleTag));
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

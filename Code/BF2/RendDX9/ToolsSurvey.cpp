// [TOOLS] tools.survey (docs/lanes/tools.md#survey), renderer side. Not part of retail BF2.
//
// Writes what render milestone 11 builds on, for the loaded level, as one text file: the sky
// dome's textures and effect, fog, sun and sky colours and the sun's elevation, the terrain's
// colormap mean colour and heights, water's techniques, environment map and area on screen,
// the environment-map system, the bundled and static material combinations, normal-map mip
// lengths and the samplers every compiled effect uses.
//
// A request waits for the next frame: an occlusion query wraps that frame's water flush, and
// once its result is in (a few frames after Present) the whole report is written at once.
// Textures are read from their managed copies; default-pool textures are reported unread.
#ifdef BF2_IMAGE_RENDDX9

#include "ToolsSurvey.h"

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "Texture.h"
#include "Ra/RaShader.h"
#include "../Math/Vec3.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/QuadPatch.h"
#include "../Geom/SkyDome.h"
#include "../Geom/GeometryTemplateManager.h"
#include "../Geom/BundledMeshTemplate.h"
#include "../Geom/BundledMeshMaterial.h"
#include "../Geom/BundledMeshStateManager.h"
#include "../Geom/StaticMeshTemplate.h"
#include "../Geom/MeshTemplate.h"
#include "../RendMisc/EnvMapSystem/EnvMapManager.h"
#include "../RendMisc/EnvMapSystem/EnvMapObject.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

namespace geom {
extern LightManager* g_LightManager;   // ModuleGeom.cpp
}

namespace dx9rend {

extern void* g_rendererBase;
void* effectD3dxHandle(void* effect);                                    // Effect.cpp
void shaderManagerForEachEffect(void (*visit)(const std::string&, void*, void*), void* context);
void effectManagerForEachEffect(void (*visit)(const std::string&, void*, void*), void* context);

bool g_toolsSurveyOn = false;

namespace {

enum Phase { Idle, Armed, Querying };

struct Survey
{
    Phase              phase;
    int                frames;        // Presents since the phase began
    int                state;         // 0 waiting, 1 written, -1 failed
    std::string        path;
    std::string        message;
    IDirect3DQuery9*   query;
    bool               waterFlushed;
    unsigned int       targetWidth, targetHeight, targetSamples;
};

Survey g_survey = { Idle, 0, 0, std::string(), std::string(), 0, false, 0, 0, 0 };

// Core/Ids.h's geom::CID_BundledMeshTemplate and CID_StaticMeshTemplate. Ids.h is not included:
// with XpLightManager.h it defines CID_LightManager twice.
const unsigned int kBundledMeshTemplate = 2265753169u;
const unsigned int kStaticMeshTemplate  = 2265753171u;

const int kArmedFrames = 8;      // Presents a request waits for a water flush
const int kQueryFrames = 30;     // Presents the query may take

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

// ---- text ----

std::string format(const char* fmt, ...)
{
    char text[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    return text;
}

std::string formatName(unsigned int f)
{
    if (f > 0xFFu)
        return std::string(1, static_cast<char>(f & 0xFF)) + static_cast<char>((f >> 8) & 0xFF)
             + static_cast<char>((f >> 16) & 0xFF) + static_cast<char>(f >> 24);
    switch (f)
    {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5:   return "R5G6B5";
    case D3DFMT_A8:       return "A8";
    case D3DFMT_L8:       return "L8";
    case D3DFMT_A8L8:     return "A8L8";
    case D3DFMT_V8U8:     return "V8U8";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_R32F:     return "R32F";
    default:              return format("fmt%u", f);
    }
}

std::string textureText(const EngineTexture* t)
{
    if (!t)
        return "-";
    static const char* const kType[] = { "?", "?", "?", "2D", "volume", "cube" };
    return format("'%s' %ux%u %s mips %u %s%s", t->name.c_str(), t->width, t->height,
                  formatName(t->format).c_str(), t->mipLevels,
                  t->resourceType < 6 ? kType[t->resourceType] : "?",
                  t->pool == 0 ? " default-pool" : "");
}

std::string vec3(const Vec3& v)
{
    return format("%.3f %.3f %.3f", v.x, v.y, v.z);
}

// ---- sections ----

void writeCamera(FILE* f)
{
    std::fprintf(f, "[camera] underwater %d (fog constants are the water's while under water)\n",
                 cameraIsUnderWater() ? 1 : 0);
}

void writeSky(FILE* f)
{
    geom::SkyDome* const sky = static_cast<geom::SkyDome*>(geom::g_SkyDome);
    if (!sky)
    {
        std::fprintf(f, "[sky] no sky dome\n");
        return;
    }
    std::fprintf(f, "[sky] template '%s' texture %s\n", sky->getSkyTemplate().c_str(),
                 textureText(sky->surveySkyTexture()).c_str());
    std::fprintf(f, "[sky] cloud layers %d %d: %s | %s\n", sky->getHasCloudLayer() ? 1 : 0,
                 sky->getHasCloudLayer2() ? 1 : 0, textureText(sky->surveyCloudTexture(0)).c_str(),
                 textureText(sky->surveyCloudTexture(1)).c_str());
    std::fprintf(f, "[sky] lighting colour %s blend %.3f; dome scale %.2f rotation %.1f\n",
                 vec3(sky->getLightingColor()).c_str(), sky->getLightingBlend(),
                 sky->getDomeScale(), sky->getDomeRotation());
    dx9rend::Effect* const effect = sky->surveyEffect();
    if (effect)
        std::fprintf(f, "[sky] effect technique '%s'\n", effectCurrentTechniqueName(effect).c_str());

    // An 8x8 grid of the texture's mean colours (rows top to bottom), from a level at most 64 wide.
    const EngineTexture* const t = sky->surveySkyTexture();
    std::vector<unsigned char> rgba;
    unsigned int w = 0, h = 0;
    std::string why;
    if (!t || !textureReadLevelRgba8(t, 0, textureLevelAtMost(t, 64), rgba, w, h, why))
    {
        std::fprintf(f, "[sky] texture grid: %s\n", t ? why.c_str() : "no texture");
        return;
    }
    std::fprintf(f, "[sky] texture grid %ux%u level, 8x8 cells, RRGGBB, top row first:\n", w, h);
    for (unsigned int gy = 0; gy < 8; ++gy)
    {
        std::string line = "[sky]  ";
        for (unsigned int gx = 0; gx < 8; ++gx)
        {
            double sum[3] = {};
            unsigned int n = 0;
            for (unsigned int y = gy * h / 8; y < (gy + 1) * h / 8 || (n == 0 && y < h); ++y)
                for (unsigned int x = gx * w / 8; x < (gx + 1) * w / 8 || x == gx * w / 8; ++x)
                {
                    if (x >= w)
                        break;
                    const unsigned char* const p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    ++n;
                }
            line += format(" %02X%02X%02X", n ? static_cast<unsigned int>(sum[0] / n) : 0,
                           n ? static_cast<unsigned int>(sum[1] / n) : 0,
                           n ? static_cast<unsigned int>(sum[2] / n) : 0);
        }
        std::fprintf(f, "%s\n", line.c_str());
    }
}

void writeFog(FILE* f)
{
    float rgb[3] = {}, water[3] = {}, waterRange[4] = {};
    postProductionGetFogColor255(rgb);
    postProductionGetWaterFogColor255(water);
    postProductionGetWaterFogStartEndAndBase(waterRange);
    const float* const range = reinterpret_cast<const float*>(raShaderConstantsAt(0xA8));
    const float* const colour = reinterpret_cast<const float*>(raShaderConstantsAt(0xBC));
    std::fprintf(f, "[fog] colour %.0f %.0f %.0f (0-255)\n", rgb[0], rgb[1], rgb[2]);
    if (range && range[0] != 0.0f)
        std::fprintf(f, "[fog] from FogRange: start %.1f end %.1f base %.3f (raw %g %g %g %g)\n",
                     -range[2] / range[0], range[3] / range[0], -range[1] / range[0],
                     range[0], range[1], range[2], range[3]);
    if (colour)
        std::fprintf(f, "[fog] FogColor constant %.3f %.3f %.3f\n", colour[0], colour[1], colour[2]);
    std::fprintf(f, "[fog] water colour %.0f %.0f %.0f, start/end/base %.1f %.1f %.3f %.3f\n",
                 water[0], water[1], water[2], waterRange[0], waterRange[1], waterRange[2],
                 waterRange[3]);
}

void writeLight(FILE* f)
{
    geom::LightManager* const lm = geom::g_LightManager;
    if (!lm)
    {
        std::fprintf(f, "[light] no light manager\n");
        return;
    }
    const Vec3 dir = lm->getSunDirection();
    const double len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const double deg = 180.0 / 3.14159265358979;
    std::fprintf(f, "[light] sun direction %s; elevation asin(-y) %.1f deg, azimuth atan2(-x,-z) %.1f deg\n",
                 vec3(dir).c_str(), len > 0 ? std::asin(-dir.y / len) * deg : 0.0,
                 std::atan2(-dir.x, -dir.z) * deg);
    std::fprintf(f, "[light] static sun %s, static sky %s, static specular %s\n",
                 vec3(lm->getStaticSunColor()).c_str(), vec3(lm->getStaticSkyColor()).c_str(),
                 vec3(lm->getStaticSpecularColor()).c_str());
    std::fprintf(f, "[light] sun %s, sun spec %s, sky %s, ambient %s, sun enabled %d\n",
                 vec3(lm->getSunColor()).c_str(), vec3(lm->getSunSpecColor()).c_str(),
                 vec3(lm->getSkyColor()).c_str(), vec3(lm->getAmbientColor()).c_str(),
                 lm->getSunEnabled() ? 1 : 0);
    std::fprintf(f, "[light] sky high %s low %s; static sky high %s low %s\n",
                 vec3(lm->getSkyColorHigh()).c_str(), vec3(lm->getSkyColorLow()).c_str(),
                 vec3(lm->getStaticSkyColorHigh()).c_str(), vec3(lm->getStaticSkyColorLow()).c_str());
    std::fprintf(f, "[light] effect sun %s, tree sun %s, tree sky %s\n",
                 vec3(lm->getEffectSunColor()).c_str(), vec3(lm->getTreeSunColor()).c_str(),
                 vec3(lm->getTreeSkyColor()).c_str());
}

void writeTerrain(FILE* f)
{
    geom::Terrain* const t = geom::terrain();
    if (!t)
    {
        std::fprintf(f, "[terrain] no terrain\n");
        return;
    }
    double colour[4] = {}, heights = 0;
    int read = 0, patches = 0;
    std::string why;
    std::map<std::string, int> formats;
    const int total = t->m_patchCount * t->m_patchCount;
    for (int p = 0; t->m_patches && p < total; ++p)
    {
        geom::QuadPatch* const patch = t->m_patches[p];
        if (!patch)
            continue;
        ++patches;
        heights += 0.5 * (patch->minHeight + patch->maxHeight);
        const EngineTexture* const map = static_cast<const EngineTexture*>(patch->colormap);
        if (!map)
            continue;
        ++formats[format("%ux%u %s mips %u", map->width, map->height,
                         formatName(map->format).c_str(), map->mipLevels)];
        double mean[4];
        if (textureMeanColour(map, 0, 16, mean, why))
        {
            for (int k = 0; k < 4; ++k)
                colour[k] += mean[k];
            ++read;
        }
    }
    std::fprintf(f, "[terrain] heights file min %.1f max %.1f; patches %d, mean patch mid-height %.1f\n",
                 t->m_minHeight, t->m_maxHeight, patches, patches ? heights / patches : 0.0);
    for (std::map<std::string, int>::const_iterator i = formats.begin(); i != formats.end(); ++i)
        std::fprintf(f, "[terrain] colormaps: %d x %s\n", i->second, i->first.c_str());
    if (read)
        std::fprintf(f, "[terrain] colormap mean colour %.1f %.1f %.1f (0-255, %d patches, a level <= 16 wide)\n",
                     colour[0] / read, colour[1] / read, colour[2] / read, read);
    else
        std::fprintf(f, "[terrain] colormap mean colour: none read (%s)\n", why.c_str());
}

void writeWater(FILE* f)
{
    geom::Terrain* const t = geom::terrain();
    if (t)
    {
        Ra::RaShader* const shaders[4] = { t->m_waterTechnique, t->m_waterDistantTechnique,
                                           t->m_waterDistantTechnique2, t->m_waterSurroundingTechnique };
        static const char* const kRole[4] = { "near", "distant", "distant2", "surrounding" };
        for (int i = 0; i < 4; ++i)
            std::fprintf(f, "[water] %s technique '%s'\n", kRole[i],
                         shaders[i] ? shaders[i]->name().c_str() : "-");
        const geom::TerrainEnvMapRecord* const env = t->m_waterEnvMap;
        std::fprintf(f, "[water] env map %s%s\n",
                     textureText(env ? static_cast<const EngineTexture*>(env->texture) : 0).c_str(),
                     env && env->isDefaultEnvMap ? " (the level's Envmaps/EnvMap0)" : "");
    }
    if (!g_survey.waterFlushed)
        std::fprintf(f, "[water] area: not measured (no water flush in the frame)\n");
    else if (!g_survey.query)
        std::fprintf(f, "[water] area: not measured (no occlusion query)\n");
}

void writeEnvMaps(FILE* f)
{
    Rend::EnvMapManager* const m = static_cast<Rend::EnvMapManager*>(Rend::g_EnvMapManager);
    if (!m)
    {
        std::fprintf(f, "[envmaps] no manager\n");
        return;
    }
    std::fprintf(f, "[envmaps] enabled %d per-pixel %d; maps %u, templates %u, objects %u; current %s\n",
                 m->m_enabled ? 1 : 0, m->m_perPixelEnabled ? 1 : 0,
                 static_cast<unsigned int>(m->m_maps.size()),
                 static_cast<unsigned int>(m->m_templates.size()),
                 static_cast<unsigned int>(m->m_objects.size()),
                 textureText(m->m_currentTexture).c_str());
    // The maps: their height range and texture kinds, and the first one as an example.
    std::map<std::string, int> kinds;
    float low = 1e30f, high = -1e30f;
    int flagged = 0;
    for (std::map<std::string, Rend::EnvMapEntry*>::const_iterator i = m->m_maps.begin();
         i != m->m_maps.end(); ++i)
        if (const Rend::EnvMapEntry* const e = i->second)
        {
            low = e->position.y < low ? e->position.y : low;
            high = e->position.y > high ? e->position.y : high;
            flagged += e->flag ? 1 : 0;
            const EngineTexture* const t = e->texture;
            ++kinds[t ? format("%ux%u %s mips %u", t->width, t->height, formatName(t->format).c_str(),
                               t->mipLevels) : std::string("no texture")];
        }
    if (!m->m_maps.empty())
    {
        std::fprintf(f, "[envmaps] maps at heights %.1f-%.1f, flagged %d; e.g. '%s'\n", low, high,
                     flagged, m->m_maps.begin()->first.c_str());
        for (std::map<std::string, int>::const_iterator i = kinds.begin(); i != kinds.end(); ++i)
            std::fprintf(f, "[envmaps] textures: %d x %s\n", i->second, i->first.c_str());
    }
    for (std::map<std::string, Rend::EnvMapObjectTemplate*, Rend::EnvMapNameLessNoCase>::const_iterator
             i = m->m_templates.begin(); i != m->m_templates.end(); ++i)
        std::fprintf(f, "[envmaps] template '%s'\n", i->first.c_str());
}

// ---- materials ----

struct Combo
{
    int materials;
    std::set<std::string> templates;
    std::vector<std::string> textures;   // up to three examples of the first texture
    float specMin, specMax, glossMin, glossMax, reflMin, reflMax;
};

void addCombo(std::map<std::string, Combo>& combos, const std::string& key, const std::string& owner,
              const std::string& texture, const geom::RaMaterialParameters& p)
{
    std::map<std::string, Combo>::iterator i = combos.find(key);
    if (i == combos.end())
    {
        Combo c;
        c.materials = 0;
        c.specMin = c.glossMin = c.reflMin = 1e30f;
        c.specMax = c.glossMax = c.reflMax = -1e30f;
        i = combos.insert(std::make_pair(key, c)).first;
    }
    Combo& c = i->second;
    ++c.materials;
    c.templates.insert(owner);
    if (c.textures.size() < 3 && !texture.empty()
        && std::find(c.textures.begin(), c.textures.end(), texture) == c.textures.end())
        c.textures.push_back(texture);
    const float spec = p.specularPower.value, gloss = p.staticGloss.value, refl = p.reflectivity.value;
    if (spec < c.specMin) c.specMin = spec;
    if (spec > c.specMax) c.specMax = spec;
    if (gloss < c.glossMin) c.glossMin = gloss;
    if (gloss > c.glossMax) c.glossMax = gloss;
    if (refl < c.reflMin) c.reflMin = refl;
    if (refl > c.reflMax) c.reflMax = refl;
}

void writeCombos(FILE* f, const char* section, const std::map<std::string, Combo>& combos)
{
    for (std::map<std::string, Combo>::const_iterator i = combos.begin(); i != combos.end(); ++i)
    {
        const Combo& c = i->second;
        std::string examples;
        for (size_t k = 0; k < c.textures.size(); ++k)
            examples += (k ? ", " : "") + c.textures[k];
        std::fprintf(f, "[%s] %s: materials %d, templates %u; specPower %g-%g staticGloss %g-%g "
                        "reflectivity %g-%g; e.g. %s\n",
                     section, i->first.c_str(), c.materials,
                     static_cast<unsigned int>(c.templates.size()), c.specMin, c.specMax,
                     c.glossMin, c.glossMax, c.reflMin, c.reflMax, examples.c_str());
    }
}

std::string textureName(const void* t)
{
    return t ? static_cast<const EngineTexture*>(t)->name : std::string();
}

// Bundled and static materials by combination, and the normal maps they use (collected into
// normalMaps for the next section).
void writeMaterials(FILE* f, std::vector<const EngineTexture*>& normalMaps)
{
    IBase* const base = geom::g_GeometryTemplateManager;
    if (!base)
    {
        std::fprintf(f, "[materials] no geometry template manager\n");
        return;
    }
    geom::GeometryTemplateRegistry* const registry =
        static_cast<geom::IGeometryTemplateManager*>(base)->getTemplateRegistry();
    if (!registry)
        return;
    std::map<std::string, Combo> bundled, statics;
    std::set<const EngineTexture*> seen;
    int bundledTemplates = 0, envTemplates = 0, staticTemplates = 0;
    static const char* const kKind[] = { "opaque", "blend", "alphatest" };
    for (geom::GeometryTemplateRegistry::const_iterator i = registry->begin(); i != registry->end(); ++i)
    {
        geom::IGeometryTemplate* const t = i->second;
        if (!t || !t->isLoaded())
            continue;
        const std::string& name = t->getName();
        if (t->getClassId() == kBundledMeshTemplate)
        {
            geom::BundledMeshTemplate* const b = static_cast<geom::BundledMeshTemplate*>(t);
            ++bundledTemplates;
            envTemplates += b->m_hasEnvironmentMap ? 1 : 0;
            for (size_t g = 0; g < b->m_materials.size(); ++g)
                for (size_t l = 0; l < b->m_materials[g].size(); ++l)
                    for (size_t m = 0; m < b->m_materials[g][l].size(); ++m)
                    {
                        geom::BundledMeshMaterial* const mat = b->m_materials[g][l][m];
                        if (!mat || !mat->m_state)
                            continue;
                        const geom::BundledMeshState* const s = mat->m_state;
                        const std::string key = format(
                            "'%s' %s textures %u%s%s%s%s%s%s%s%s", s->m_name.c_str(),
                            mat->m_kind >= 0 && mat->m_kind < 3 ? kKind[mat->m_kind] : "kind?",
                            static_cast<unsigned int>(s->m_textures.size()),
                            s->m_normalMap ? " normal" : "", s->m_envMap ? " env" : "",
                            s->m_colorMapGloss ? " colorGloss" : "", s->m_alphaTest ? " alphaTest" : "",
                            s->m_alphaOne ? " alphaOne" : "", s->m_cockpit ? " cockpit" : "",
                            s->m_animated ? " animated" : "", s->m_extraMap ? " extra" : "");
                        addCombo(bundled, key, name,
                                 s->m_textures.empty() ? std::string() : textureName(s->m_textures[0]),
                                 geom::bundledMeshMaterialParameters(mat));
                        if (s->m_normalMap && s->m_textures.size() > 1 && s->m_textures[1]
                            && seen.insert(s->m_textures[1]).second)
                            normalMaps.push_back(s->m_textures[1]);
                    }
        }
        else if (t->getClassId() == kStaticMeshTemplate)
        {
            geom::StaticMeshTemplate* const s = static_cast<geom::StaticMeshTemplate*>(t);
            ++staticTemplates;
            for (size_t g = 0; g < s->m_materials.size(); ++g)
                for (size_t l = 0; l < s->m_materials[g].size(); ++l)
                    for (size_t m = 0; m < s->m_materials[g][l].size(); ++m)
                    {
                        geom::StaticMeshMaterial& mat = s->m_materials[g][l][m];
                        const geom::ShaderMaterial* const sm = mat.m_shaderMaterial;
                        if (!sm)
                            continue;
                        std::string bits;
                        geom::staticMeshShaderBitsToName(bits, geom::g_shaderComboFlags[sm->shaderCombo & 15u]);
                        const std::string key = format("'%s' combo %u (%s) alpha %u textures %u",
                                                       sm->name.c_str(), sm->shaderCombo, bits.c_str(),
                                                       mat.m_alphaMode, sm->textureCount);
                        geom::RaMaterialParameters& p = geom::staticMeshMaterialParameters(&mat);
                        addCombo(statics, key, name, textureName(sm->textures[1]), p);
                        const EngineTexture* const normal = static_cast<const EngineTexture*>(p.normalMap.value);
                        if (normal && seen.insert(normal).second)
                            normalMaps.push_back(normal);
                    }
        }
    }
    std::fprintf(f, "[bundled] loaded templates %d, with an environment map %d\n", bundledTemplates,
                 envTemplates);
    writeCombos(f, "bundled", bundled);
    std::fprintf(f, "[static] loaded templates %d\n", staticTemplates);
    writeCombos(f, "static", statics);
}

// Mean length of the decoded normal (rgb * 2 - 1) per mip, from the first level <= 256 wide down.
void writeNormalMaps(FILE* f, const std::vector<const EngineTexture*>& maps)
{
    std::fprintf(f, "[normalmaps] %u distinct; mean |rgb*2-1| per level (width:length)\n",
                 static_cast<unsigned int>(maps.size()));
    const size_t kMax = 40;
    for (size_t i = 0; i < maps.size() && i < kMax; ++i)
    {
        const EngineTexture* const t = maps[i];
        std::string line = format("[normalmaps] '%s' %s:", t->name.c_str(), formatName(t->format).c_str());
        for (unsigned int level = textureLevelAtMost(t, 256); level < t->mipLevels; ++level)
        {
            std::vector<unsigned char> rgba;
            unsigned int w = 0, h = 0;
            std::string why;
            if (!textureReadLevelRgba8(t, 0, level, rgba, w, h, why))
            {
                line += " " + why;
                break;
            }
            double sum = 0;
            for (size_t k = 0; k < rgba.size(); k += 4)
            {
                const double x = rgba[k] / 127.5 - 1, y = rgba[k + 1] / 127.5 - 1, z = rgba[k + 2] / 127.5 - 1;
                sum += std::sqrt(x * x + y * y + z * z);
            }
            line += format(" %u:%.3f", w, rgba.empty() ? 0.0 : sum / (rgba.size() / 4));
        }
        std::fprintf(f, "%s\n", line.c_str());
    }
    if (maps.size() > kMax)
        std::fprintf(f, "[normalmaps] (first %u listed)\n", static_cast<unsigned int>(kMax));
}

// ---- effects ----

struct EffectWalk
{
    FILE* file;
    const char* cache;
    unsigned int psUsed[16];   // effects using each pixel sampler
    unsigned int vsUsed[4];
    unsigned int effects;
    unsigned int withoutCode;  // effects whose passes keep no bytecode (D3DXFX_NOT_CLONEABLE)
    int boundMax;              // the largest sampler-parameter bound among those
    std::string boundMaxName;
    int constMax;              // [RENDER] the highest pixel float4 register an effect's own constant uses
    std::string constMaxName;
    unsigned int collisions;   // effect constants inside the engine-owned registers (contract 36)
};

// [RENDER] The engine-owned pixel registers' names (RendDX9Base.h, kEngineRegisterBegin..End).
bool engineOwnedConstant(const char* name)
{
    static const char* const kOwned[] = { "RaWaterMedium", "RaSunStatic", "RaWaterFoam", "RaCloudShadow", "RaWaterSsr",
        "RaSkySH", "RaSunFactor", "RASM", "RaLodDither", "RaAerial" };
    for (size_t i = 0; name && i < sizeof(kOwned) / sizeof(kOwned[0]); ++i)
        if (std::strcmp(name, kOwned[i]) == 0)
            return true;
    return false;
}

// The highest pixel float4 register this shader's own constants use (not the engine's), and
// those of them inside the engine-owned range.
void addConstants(const DWORD* function, int& highest, std::set<std::string>& inEngineRange)
{
    ID3DXConstantTable* table = 0;
    if (!function || FAILED(D3DXGetShaderConstantTable(function, &table)) || !table)
        return;
    D3DXCONSTANTTABLE_DESC desc;
    if (SUCCEEDED(table->GetDesc(&desc)))
        for (UINT i = 0; i < desc.Constants; ++i)
        {
            D3DXHANDLE const h = table->GetConstant(0, i);
            D3DXCONSTANT_DESC c;
            UINT count = 1;
            if (!h || FAILED(table->GetConstantDesc(h, &c, &count)) || c.RegisterSet != D3DXRS_FLOAT4
                || engineOwnedConstant(c.Name))
                continue;
            const int last = static_cast<int>(c.RegisterIndex + c.RegisterCount) - 1;
            if (last > highest)
                highest = last;
            if (last >= static_cast<int>(kEngineRegisterBegin)
                && c.RegisterIndex < kEngineRegisterEnd)
                inEngineRange.insert(format("%s:c%u+%u", c.Name ? c.Name : "?", c.RegisterIndex,
                                            c.RegisterCount));
        }
    table->Release();
}

void addSamplers(const DWORD* function, unsigned int& mask, std::set<std::string>& cubes)
{
    if (!function)
        return;
    ID3DXConstantTable* table = 0;
    if (FAILED(D3DXGetShaderConstantTable(function, &table)) || !table)
        return;
    D3DXCONSTANTTABLE_DESC desc;
    if (SUCCEEDED(table->GetDesc(&desc)))
        for (UINT i = 0; i < desc.Constants; ++i)
        {
            D3DXHANDLE const h = table->GetConstant(0, i);
            D3DXCONSTANT_DESC c;
            UINT count = 1;
            if (!h || FAILED(table->GetConstantDesc(h, &c, &count)) || c.RegisterSet != D3DXRS_SAMPLER)
                continue;
            for (UINT r = 0; r < c.RegisterCount && c.RegisterIndex + r < 32; ++r)
                mask |= 1u << (c.RegisterIndex + r);
            if (c.Type == D3DXPT_SAMPLERCUBE)
                cubes.insert(format("%s:s%u", c.Name ? c.Name : "?", c.RegisterIndex));
        }
    table->Release();
}

std::string maskText(unsigned int mask)
{
    std::string text;
    for (int i = 0; i < 32; ++i)
        if (mask & (1u << i))
            text += format("%s%d", text.empty() ? "" : ",", i);
    return text.empty() ? std::string("-") : text;
}

void visitEffect(const std::string& name, void* d3dx, void* context)
{
    EffectWalk& walk = *static_cast<EffectWalk*>(context);
    ID3DXEffect* const fx = static_cast<ID3DXEffect*>(d3dx);
    D3DXEFFECT_DESC desc;
    if (FAILED(fx->GetDesc(&desc)))
        return;
    unsigned int ps = 0, vs = 0;
    std::set<std::string> cubes, cubeParams;
    unsigned int samplerParams = 0;
    std::set<const DWORD*> seen;
    unsigned int functions = 0;
    int constHighest = -1;
    std::set<std::string> inEngineRange;
    for (UINT t = 0; t < desc.Techniques; ++t)
    {
        D3DXHANDLE const technique = fx->GetTechnique(t);
        D3DXTECHNIQUE_DESC td;
        if (!technique || FAILED(fx->GetTechniqueDesc(technique, &td)))
            continue;
        for (UINT p = 0; p < td.Passes; ++p)
        {
            D3DXHANDLE const pass = fx->GetPass(technique, p);
            D3DXPASS_DESC pd;
            if (!pass || FAILED(fx->GetPassDesc(pass, &pd)))
                continue;
            functions += (pd.pPixelShaderFunction || pd.pVertexShaderFunction) ? 1 : 0;
            if (pd.pPixelShaderFunction && seen.insert(pd.pPixelShaderFunction).second)
            {
                addSamplers(pd.pPixelShaderFunction, ps, cubes);
                addConstants(pd.pPixelShaderFunction, constHighest, inEngineRange);
            }
            if (pd.pVertexShaderFunction && seen.insert(pd.pVertexShaderFunction).second)
            {
                std::set<std::string> vsCubes;
                addSamplers(pd.pVertexShaderFunction, vs, vsCubes);
            }
        }
    }
    for (UINT i = 0; i < desc.Parameters; ++i)
    {
        D3DXHANDLE const h = fx->GetParameter(0, i);
        D3DXPARAMETER_DESC pd;
        if (!h || FAILED(fx->GetParameterDesc(h, &pd)))
            continue;
        if (pd.Type == D3DXPT_TEXTURECUBE)
            cubeParams.insert(pd.Name ? pd.Name : "?");
        if (pd.Type >= D3DXPT_SAMPLER && pd.Type <= D3DXPT_SAMPLERCUBE)
            samplerParams += pd.Elements ? pd.Elements : 1;
    }
    int highest = -1;
    for (int i = 0; i < 16; ++i)
        if (ps & (1u << i))
        {
            highest = i;
            ++walk.psUsed[i];
        }
    for (int i = 0; i < 4; ++i)
        if (vs & (1u << i))
            ++walk.vsUsed[i];
    ++walk.effects;
    std::string cubeText, paramText;
    for (std::set<std::string>::const_iterator i = cubes.begin(); i != cubes.end(); ++i)
        cubeText += (cubeText.empty() ? "" : ",") + *i;
    for (std::set<std::string>::const_iterator i = cubeParams.begin(); i != cubeParams.end(); ++i)
        paramText += (paramText.empty() ? "" : ",") + *i;
    if (!functions)
    {
        // No bytecode to read: the sampler parameters bound what its shaders can use. Samplers
        // without an explicit register are numbered from s0 per shader, so a shader of this
        // effect uses at most s0..s(params-1) besides explicit registers (s13, s15).
        ++walk.withoutCode;
        if (static_cast<int>(samplerParams) - 1 > walk.boundMax)
        {
            walk.boundMax = static_cast<int>(samplerParams) - 1;
            walk.boundMaxName = name;
        }
        std::fprintf(walk.file, "[effects] %s '%s': techniques %u, no bytecode; sampler params %u "
                                "(auto registers at most s0-s%d); cube params %s\n",
                     walk.cache, name.c_str(), desc.Techniques, samplerParams,
                     static_cast<int>(samplerParams) - 1, paramText.empty() ? "-" : paramText.c_str());
        return;
    }
    if (constHighest > walk.constMax)
    {
        walk.constMax = constHighest;
        walk.constMaxName = name;
    }
    std::string collisionText;
    for (std::set<std::string>::const_iterator i = inEngineRange.begin(); i != inEngineRange.end(); ++i)
        collisionText += (collisionText.empty() ? "" : ",") + *i;
    walk.collisions += static_cast<unsigned int>(inEngineRange.size());
    std::fprintf(walk.file, "[effects] %s '%s': techniques %u, ps samplers %s (highest %d), vs samplers %s; "
                            "sampler params %u; cube samplers %s; cube params %s; ps constants to c%d%s%s\n",
                 walk.cache, name.c_str(), desc.Techniques, maskText(ps).c_str(), highest,
                 maskText(vs).c_str(), samplerParams, cubeText.empty() ? "-" : cubeText.c_str(),
                 paramText.empty() ? "-" : paramText.c_str(), constHighest,
                 collisionText.empty() ? "" : "; IN THE ENGINE'S REGISTERS ", collisionText.c_str());
}

void writeEffects(FILE* f)
{
    EffectWalk walk = {};
    walk.file = f;
    walk.boundMax = -1;
    walk.constMax = -1;
    walk.cache = "legacy";
    shaderManagerForEachEffect(&visitEffect, &walk);
    walk.cache = "ra";
    effectManagerForEachEffect(&visitEffect, &walk);
    std::string ps, vs;
    for (int i = 0; i < 16; ++i)
        ps += format(" s%d:%u", i, walk.psUsed[i]);
    for (int i = 0; i < 4; ++i)
        vs += format(" s%d:%u", i, walk.vsUsed[i]);
    std::fprintf(f, "[effects] %u effects with bytecode; how many use each pixel sampler:%s\n",
                 walk.effects - walk.withoutCode, ps.c_str());
    std::fprintf(f, "[effects] effects using each vertex sampler:%s\n", vs.c_str());
    std::fprintf(f, "[effects] %u effects without bytecode; their largest sampler-parameter bound "
                    "s%d ('%s')\n", walk.withoutCode, walk.boundMax, walk.boundMaxName.c_str());
    std::fprintf(f, "[effects] highest pixel register of an effect's own constants c%d ('%s'); "
                    "engine-owned c%u-c%u; %u effect constants inside them\n", walk.constMax,
                 walk.constMaxName.c_str(), kEngineRegisterBegin, kEngineRegisterEnd - 1,
                 walk.collisions);
}

void writeReport()
{
    FILE* f = 0;
    if (fopen_s(&f, g_survey.path.c_str(), "w") != 0 || !f)
    {
        g_survey.state = -1;
        g_survey.message = "cannot write " + g_survey.path;
        return;
    }
    std::fprintf(f, "# tools.survey (render milestone 11 task 1)\n");
    writeCamera(f);
    writeSky(f);
    writeFog(f);
    writeLight(f);
    writeTerrain(f);
    writeWater(f);
    if (g_survey.waterFlushed && g_survey.query)
    {
        DWORD samples = 0;
        const bool got = g_survey.query->GetData(&samples, sizeof(samples), 0) == S_OK;
        const double all = static_cast<double>(g_survey.targetWidth) * g_survey.targetHeight
                         * (g_survey.targetSamples ? g_survey.targetSamples : 1);
        if (got)
            std::fprintf(f, "[water] area: %lu samples of %ux%u x%u = %.2f %% of the screen\n",
                         static_cast<unsigned long>(samples), g_survey.targetWidth,
                         g_survey.targetHeight, g_survey.targetSamples ? g_survey.targetSamples : 1,
                         all > 0 ? 100.0 * samples / all : 0.0);
        else
            std::fprintf(f, "[water] area: query result not available\n");
    }
    writeEnvMaps(f);
    std::vector<const EngineTexture*> normalMaps;
    writeMaterials(f, normalMaps);
    writeNormalMaps(f, normalMaps);
    writeEffects(f);
    std::fclose(f);
    g_survey.state = 1;
    g_survey.message = g_survey.path;
}

void finish()
{
    writeReport();
    if (g_survey.query)
        g_survey.query->Release();
    g_survey.query = 0;
    g_survey.phase = Idle;
    g_toolsSurveyOn = false;
}

} // namespace

void toolsSurveyWaterBegin()
{
    if (g_survey.phase != Armed)
        return;
    g_survey.waterFlushed = true;
    IDirect3DDevice9* const dev = device();
    if (!dev)
        return;
    IDirect3DSurface9* target = 0;
    if (SUCCEEDED(dev->GetRenderTarget(0, &target)) && target)
    {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(target->GetDesc(&desc)))
        {
            g_survey.targetWidth = desc.Width;
            g_survey.targetHeight = desc.Height;
            g_survey.targetSamples = desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES
                                   ? static_cast<unsigned int>(desc.MultiSampleType) : 1;
        }
        target->Release();
    }
    if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_survey.query)) && g_survey.query)
        g_survey.query->Issue(D3DISSUE_BEGIN);
}

void toolsSurveyWaterEnd()
{
    if (g_survey.phase != Armed || !g_survey.waterFlushed)
        return;
    if (g_survey.query)
        g_survey.query->Issue(D3DISSUE_END);
    g_survey.phase = Querying;
    g_survey.frames = 0;
}

void toolsSurveyPresented()
{
    if (g_survey.phase == Armed)
    {
        if (++g_survey.frames >= kArmedFrames)
            finish();   // no water flush: the report says so
        return;
    }
    if (g_survey.phase != Querying)
        return;
    if (g_survey.query)
    {
        DWORD samples = 0;
        if (g_survey.query->GetData(&samples, sizeof(samples), D3DGETDATA_FLUSH) == S_FALSE
            && ++g_survey.frames < kQueryFrames)
            return;
    }
    finish();
}

void toolsRequestSurvey(const char* path)
{
    if (g_survey.query)
        g_survey.query->Release();
    g_survey.query = 0;
    if (!path)
    {
        g_survey.phase = Idle;
        g_survey.state = -1;
        g_survey.message = "cancelled";
        g_toolsSurveyOn = false;
        return;
    }
    g_survey.phase = Armed;
    g_survey.frames = 0;
    g_survey.state = 0;
    g_survey.path = path;
    g_survey.message.clear();
    g_survey.waterFlushed = false;
    g_survey.targetWidth = g_survey.targetHeight = g_survey.targetSamples = 0;
    g_toolsSurveyOn = true;
}

int toolsSurveyState(const char*& message)
{
    message = g_survey.message.c_str();
    return g_survey.state;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

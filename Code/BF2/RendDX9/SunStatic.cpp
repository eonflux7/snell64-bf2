// [RENDER] Render milestone 14: the run-time sun term past the cascades. Not part of retail BF2.
// The lightmaps' baked sun (visibility x N.L for the level's authored sun) is replaced by N.L for the
// round's sun everywhere, shadowed past the cascades by the static layers this file draws once per
// sun. RaSunStatic (contract 36, pixel registers c180..c187) is laid out in the shaders'
// RaSunShadow.fxh; docs/lanes/render.md, milestone 14, has the design.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../Geom/XpLightManager.h"
#include "../Game/Objects/Visual/LightSource.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }

namespace dx9rend {

extern void* g_rendererBase;

namespace {

SunStaticSettings g_settings = { false, 4096u, 0.5f, 0.3f, 1024u };
IDirect3DTexture9* g_atlas = 0;   // R32F: the static cascade, the shadow height below it

IDirect3DDevice9* device()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->device;
}

} // namespace

SunStaticSettings& sunStaticSettings()
{
    return g_settings;
}

IDirect3DTexture9* rendererSunStaticAtlas()
{
    return g_atlas;
}

void rendererSunStaticInvalidate()
{
}

void rendererSunStaticRelease()
{
    if (g_atlas && g_rendererBase)
    {
        device()->SetTexture(kWaterSceneSampler, 0);
        device()->SetTexture(3, 0);
    }
    if (g_atlas)
        g_atlas->Release();
    g_atlas = 0;
}

void rendererSunStaticBind(DWORD stage)
{
    if (!g_rendererBase)
        return;
    IDirect3DDevice9* const d = device();
    d->SetTexture(stage, g_atlas);
    d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    d->SetSamplerState(stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

void rendererSunStaticFrame(float* constants)
{
    std::memset(constants, 0, sizeof(float) * 4 * kSunStaticVec4s);
    if (!geom::g_LightManager)
        return;
    world::LightSource* const sky = geom::g_LightManager->getSkyLight();
    if (!sky)
        return;
    const Vec3& d = sky->getDirection();   // the direction the sunlight travels
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (!(length > 1e-4f))
        return;
    // [4]: toward the sun; w 1 while the run-time sun term replaces the baked one
    constants[16] = -d.x / length;
    constants[17] = -d.y / length;
    constants[18] = -d.z / length;
    constants[19] = g_settings.bakedFallback ? 0.0f : 1.0f;
    rendererSunStaticBind(kWaterSceneSampler);
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

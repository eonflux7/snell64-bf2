# Renderer: shader contract

Part of [the renderer as built](README.md).

## Shader contract

The contract has a version, `RA_CONTRACT_VERSION` in `RendDX9Base.h`. The engine passes it as the
define `RA_CONTRACT` to every effect it compiles: Ra effects get it with their profile defines,
every other effect through the ShaderManager define table. The shaders' core header stops with
`#error` when it is missing or older than they need, so a shader set never runs on an engine
without its contract. Raise the version in the change that alters what the shaders may rely on:
its row goes in [contract-versions.md](contract-versions.md), where every version's additions
are listed, and the tables below are updated to match. Below: what the shaders may rely on now.

Samplers 9–15 are reserved for renderer frame resources (and 2–8 in water effects, contracts 17–18, 36 and 38), and pixel registers
c176–c223 for `RaWaterMedium`, `RaSunStatic`, `RaWaterFoam`, `RaCloudShadow`, `RaWaterSsr`, `RaSkySH`, `RaSunFactor`, `RASM`, `RaLodDither` and `RaAerial`. No effect may use them for anything else.
Every other sampler states `SRGBTexture` in its `sampler_state`: TRUE where it reads colour (the
hardware decodes before filtering), FALSE for data and outside the scene. D3D9 sampler state carries
over between effects, so neither value is left to default. The engine binds s9–s15 (and s6–s8 for water) with it FALSE.

Every `sampler_state` also states its filter. A material sampler (a colour, normal or detail texture
mapped onto world geometry) is 16x anisotropic: min `ANISOTROPIC`, mag and mip `LINEAR`. A data
sampler (lightmaps, light and blend maps, cube and lookup maps, render targets, UI) states point or
linear. The engine defines no filter: retail's caps- and setting-dependent `FILTER_*` defines and
`IS_NV4X` are gone, so the texture filter quality setting changes nothing and the effect cache
directory no longer carries it (`_3153_0`).

### Samplers and registers

| Slot | Resource | Format | Bound |
|---|---|---|---|
| s2 | The open-water map and its fetch table in water effects (contract 38) | `A16B16G16R16F` | During the water queue only, while `RaWaterFoam.w` > 0 |
| s3 | The sun's static layers in water effects (contract 36) | `R32F` | During the water queue only |
| s4–s5 | Water's foam maps, the two finer wave cascades (contract 18) | `R16F`, mipmapped | During the water queue only, while `RaWaterFoam.x` > 0 |
| s6–s8 | Water's slope maps, one per wave cascade (contract 17) | `A16B16G16R16F`, mipmapped | During the water queue only, while `RaWaterSsr.w` > 0 |
| s9 | Aerial-perspective atlas (2080x32: 32 in-scatter and 32 transmittance slices of 32x32 screen tiles, then the sky block, contract 27) | `A16B16G16R16F` | After the atlas pass (start of the depth prepass), rest of the frame; read only while `RaAerial.w` > 0 |
| s10 | Sky cube: mip m GGX-prefiltered for perceptual roughness m / 5 (contract 10) | `A16B16G16R16F` cube, 128 per face, 6 mips | From the sky cube's first fit; unbound before it (`RaSkySH[6].w` 0) |
| s11 | Split-sum BRDF table: u n.v, v perceptual roughness, r scale and g bias on F0 (contract 10) | 64x64 `A16B16G16R16F` | As s10 |
| s12 | Scene colour copy for water's reflections (contract 12) | The scene's (`A16B16G16R16F`), mipmapped | During the water queue only, while `RaWaterSsr.x` > 0 |
| s12 | The sun's static layers, the static cascade over the shadow height (contract 36) | `R32F` | The world pass outside the water queue; read while `RaSunStatic[5].zw` > 0 |
| s13 | Sun cascade atlas | `R32F` | From the cascade pass, rest of the frame |
| s14 | Cloud shadow map (contract 16) | `A16B16G16R16F` | From the clouds' frame step (before the prepass), rest of the frame; read while `RaCloudShadow[0].w` > 0 |
| s15 | Linear view depth | `R32F` | After the prepass, rest of the frame; particles read it only while `RaSoftParticles.x` > 0 |

Pixel registers c176–c223 are the engine's: effects' own writes to them are dropped
(`kEngineRegisterBegin`, `kEngineRegisterEnd`).

| Registers | Constant | Contract | Set |
|---|---|---|---|
| c176–c179 | `RaWaterMedium`: the water type's attenuation, backscatter, surf sediment, caustics | 42–44 | Every frame |
| c180–c187 | `RaSunStatic`: the round's sun, the static cascade's rows, the shadow height's mapping | 36 | Main world pass, published with `RASM`; zero elsewhere |
| c188 | `RaWaterFoam`: foam bound, break depth, loop seconds, 1 / the open-water map's side | 18, 38, 41 | While the water queue draws; zero otherwise |
| c189–c190 | `RaCloudShadow` | 16 | Main world pass, published with `RASM`; zero elsewhere |
| c191 | `RaWaterSsr`: the scene copy's size; w 1 with the slope maps | 12, 17 | While the water queue draws, above water; zero otherwise |
| c192–c198 | `RaSkySH`: the sky's irradiance / π | 10, 11 | Once fitted; zero before |
| c199 | `RaSunFactor`: the surfaces' sun over the authored | 26 | Every pass |
| c200–c221 | `RASM`: the sun cascades | — | Once a frame |
| c222 | `RaLodDither`: the LOD crossfade | — | Before every entry draw |
| c223 | `RaAerial`: the aerial-perspective atlas's mapping | 8, 9 | World view; zero in first-person passes and under water |

### Techniques and parameters

Techniques the engine looks up by name:

| Technique | In | Draws |
|---|---|---|
| `RaDepthPrepass` | Ra mesh effects, terrain | Linear depth to COLOR0 |
| `RaShadowDepth` | Ra mesh effects, terrain | Light depth into the atlas, with `RaShadowViewProj` |
| `RaTerrain` | Terrain | Every detail layer in one pass (contract 5, [Terrain](terrain.md#terrain)) |

Passes the submission layer draws (`RaDepthPrepass` and `RaShadowDepth` of the Ra mesh effects)
take every render and sampler state as a constant, except three a material decides, which follow
these parameters per draw: `RaAlphaTest` (`bool`, ALPHATESTENABLE), `RaAlphaRef` (`int` 0–255,
ALPHAREF), `RaDepthWrite` (`bool`, ZWRITEENABLE). A pass that fixes one of them (leaves:
`AlphaTestEnable = TRUE`) keeps its own value. They set no fixed-function state, and a texture
reaches them only through a sampler's `Texture = (param)`.

Parameters the engine sets by semantic:

| Semantic | Type | Set on |
|---|---|---|
| `RaShadowViewProj` | `float4x4` | Ra effects drawing `RaShadowDepth`: the cascade being drawn |
| `RASM` | `float4[22]` | The particle effects at every bind (`rendererBindParticleLighting`), which sample per vertex. Pixel shaders read it from c200–c221 instead and nothing sets it on them |
| `SUNCOLOR` (`TerrainSunColor` in the Ra constants) | `float3` | Terrain, road and undergrowth effects: the level's one sun, `Lightmanager.staticSunColor` as authored (undergrowth times its `LightingScale`). Retail passed `terrain.sunColor / 4`, balanced against the statics by hand per level |
| `RaSunColor`, `RaSkyColor` | `float3` | The four particle effects (`rendererBindParticleLighting`, called by each particle renderer as it binds its effect): `Lightmanager.staticSunColor` and `staticSkyColor` as authored, the one sun and sky every lit surface uses |
| `RaSunDirection` | `float3` | The particle effects: unit vector toward the sun (`-Lightmanager.sunDirection`) |
| `RaSunCascadeAtlas` | `texture` | The particle effects: the cascade atlas, which they sample per vertex. A vertex shader cannot read s13, so the atlas reaches them as a texture parameter and D3DX binds the vertex sampler |
| `RaSoftParticles` | `float4` | The particle effects: `.x` 1 / `renderer.softParticleDistance` while the main world pass draws over this frame's prepass depth, else 0 (no fade); `.zw` 1 / the depth target's size |
| `TemplateParameters` `color1.a` (light factor) | `float` | The particle templates, as retail, except a template that sets no light factor: `-1 - Lightmanager.defaultEffectLightAffectionFactor` (`particleTemplateUnsetLightAffection`). The shaders light it fully; thermals read back the level default |
| `EffectSunColor`, `EffectShadowColor` | `float3` | The particle effects, as retail: read only by the thermals path, which keeps PR's particle formula, and (`EffectSunColor`) as the fill scale of the `ShowFill` techniques |
| `Lights[0].color` (sun record), `SunColor` (decals) | `float3` | Bundled meshes (cockpits included), skinned meshes, trees and decals: the same one sun, `Lightmanager.staticSunColor`. Retail gave them `Lightmanager.sunColor`, the cockpit gate's colour, `treeSunColor` or the sky light's colour, each authored against a separate ambient (on Kashan the dynamic sun is 0.6 against a static 0.73 doubled). Unscaled: retail also multiplied bundled and skinned meshes' sun by the baked simple shadow map, which the cascades replace and which blacked out meshes under roofs once the sky came from the sun |

## 6. Shader audit and cleanup

An inventory of the shader set (every effect and header, its techniques and permutations, which of
them a PR session loads, what later milestones replace) lives with the private shaders. Liveness
came from `BF2_DRAW_STATS` ([dev.md](../lanes/dev.md)) on maps that between them cover water, foliage,
buildings and vehicles. What it removed from the engine:
- The toggles fixed when milestones 1–4 were accepted (reversed-Z, log depth, gamma-space lighting)
  and their dead branches.
- BF2's directional (sun) shadow maps: the directional pass in `XpLightManager.cpp`, its records and
  every receiver. That is the mesh shadow permutations (bundled and skinned mesh `hasshadow` /
  `hasshadowocclusion` / `hashemiocclusion`, static mesh `shadow`, the `*Shadowed` tree
  techniques), terrain water's record matrices, undergrowth's `_ds` techniques, overgrowth's record
  attach, and the decal renderer's shadowed batch kinds (decals draw pass 0 only).
- BF2's point and spot shadow maps. Retail kept shadow records only for lights with a non-zero
  shadow policy, which is zero outside the expansion pack, so a PR session never drew this path.
  Deleted: the records, the shadow atlas and its device-lost listener, the casters'
  `DrawShadowMap` draws on bundled, skinned and static meshes (with the shadow effects they
  loaded), the static mesh lightmap silhouette, and the record receivers in static meshes, terrain
  (the shadow-range gather and batch draws, the spot-light pass) and overgrowth. The XP light
  coordinator stays: it still selects the point and spot lights that light geometry.
- Kept as empty bodies for the vtable layout: the LightManager's `renderShadowMaps` and
  `registerShadowEffect`, the meshes' `drawShadowMap` / `drawSilhouette`, overgrowth's
  `collectShadowGeometry`. The shadow-map console setters only store their value. The deleted pass
  also bound the scene depth-stencil and colour target 0 on exit each frame; the renderer now binds
  them itself where it was called (`dc97ef7b`), and the sun cascades clear their standard-depth mark
  when they restore the scene depth (`e7623d07`).
- One terrain path. `Low_Terrain` had no `RaDepthPrepass` or `RaShadowDepth` technique, so the
  engine no longer enters it: not at load, not from the terrain-quality video option, not as a
  fallback. Its `m_lowQuality` branches went in milestone 10 ([Terrain](terrain.md#terrain)).

// [RENDER] Render milestone 14: the run-time sun term past the cascades. Not part of retail BF2.
// The lightmaps' baked sun (visibility x N.L for the level's authored sun) is replaced by N.L for the
// round's sun everywhere, shadowed past the cascades by two layers drawn once per sun into one R32F
// atlas (static cascade on top, shadow height below it):
//
// - The static cascade: an orthographic light-space map over the level's box (the map's square,
//   its heights plus headroom for buildings and trees), the light depth of the nearest static
//   caster: terrain (always full detail) and static meshes of objects without mobile physics, at
//   the LOD they take at a fixed distance (one camera pixel per static texel), so the map never
//   depends on where the camera was. Overgrowth streams around the camera and is left out.
// - The shadow height: per texel of a top-down square over the far ground's coarse heights, the
//   world height below which terrain toward the sun blocks it, marched on the CPU. On the map only
//   terrain outside it counts (the static cascade holds the map's own); past the map all of it.
//
// Both are redrawn when the sun, the level, the far ground's heights or a setting change, and on
// rendererSunStaticInvalidate. RaSunStatic (contract 36, pixel registers c180..c187) is laid out in
// the shaders' RaSunShadow.fxh; docs/render/shadows.md, milestone 14, has the design.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "GpuTimers.h"
#include "ToolsAtmosphere.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Frustum.h"
#include "../Math/Mat4.h"
#include "../Geom/XpLightManager.h"
#include "../Geom/Terrain.h"
#include "../Geom/ObjectDrawer.h"
#include "../Geom/FarGround.h"
#include "../Game/Objects/Visual/LightSource.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }

namespace dx9rend {

extern void* g_rendererBase;
unsigned int raHashString(const std::string& s);

namespace {

SunStaticSettings g_settings = { 4096u, 1.0f, 1.0f, 512u };
const float kHeadroom = 150.0f;       // m above the map's highest ground the casters may reach
const float kFootroom = 20.0f;        // m below its lowest

IDirect3DTexture9* g_atlas = 0;       // R32F, size x (size + heightSize)
unsigned int g_atlasSize = 0;
unsigned int g_atlasHeightSize = 0;

// What the layers were drawn for; a difference redraws them.
struct Key
{
    const void* terrain;
    float sun[3];
    unsigned int resolution;
    unsigned int heightSize;
    const float* heights;
    unsigned int heightGeneration;
};
Key g_drawn;   // zero-initialised (static storage)
bool g_invalid = true;
bool g_castersChanged = false;                         // the static cascade only
std::chrono::steady_clock::time_point g_staticDrawn;   // when it was last drawn
float g_depthPerTexel = 0.0f;   // the static cascade's light depth over one texel

// The drawn layers' constants (rows [0..3], [5].zw, [6], [7]); the frame adds [4] and [5].xy.
float g_layers[kSunStaticVec4s * 4] = {};

IDirect3DDevice9* device()
{
    return static_cast<RendDX9Base*>(g_rendererBase)->device;
}

struct V3
{
    float x, y, z;
};
inline float dot3(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross3(const V3& a, const V3& b)
{
    const V3 c = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return c;
}
inline V3 norm3(const V3& a)
{
    const float l = std::sqrt(dot3(a, a));
    const V3 n = { a.x / l, a.y / l, a.z / l };
    return n;
}

bool ensureAtlas()
{
    const unsigned int size = g_settings.resolution;
    const unsigned int heightSize = (std::min)(g_settings.heightSize, size);
    if (g_atlas && g_atlasSize == size && g_atlasHeightSize == heightSize)
        return true;
    rendererSunStaticRelease();
    const HRESULT hr = device()->CreateTexture(size, size + heightSize, 1, D3DUSAGE_RENDERTARGET,
                                               D3DFMT_R32F, D3DPOOL_DEFAULT, &g_atlas, 0);
    std::fprintf(stderr, "[SUNSTATIC] atlas %ux%u R32F (static %u, height %u): hr=0x%08lX\n", size,
                 size + heightSize, size, heightSize, static_cast<unsigned long>(hr));
    if (hr < 0)
    {
        g_atlas = 0;
        return false;
    }
    g_atlasSize = size;
    g_atlasHeightSize = heightSize;
    return true;
}

// The static cascade into the atlas's top square; fills rows [0..3] and [5].z. False when it could
// not draw (the depth target), leaving it uncovered.
bool drawStaticCascade(geom::Terrain* terrain, const V3& forward, float time)
{
    const unsigned int size = g_atlasSize;
    const float half = 0.5f * static_cast<float>(terrain->m_patchCount * terrain->m_patchSize)
                     * terrain->m_primaryWorldScale.x;
    if (!(half > 0.0f))
        return false;
    const float lowY = terrain->m_minHeight - kFootroom;
    const float highY = terrain->m_maxHeight + kHeadroom;

    // The light basis, as the cascades': forward along the sunlight, right and up across it.
    const V3 worldUp = { 0.0f, 1.0f, 0.0f };
    const V3 worldZ = { 0.0f, 0.0f, 1.0f };
    const V3 right = norm3(cross3(std::fabs(forward.y) > 0.99f ? worldZ : worldUp, forward));
    const V3 up = cross3(forward, right);

    // The level's box in light coordinates.
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f, minZ = 1e30f, maxZ = -1e30f;
    for (int i = 0; i < 8; ++i)
    {
        const V3 p = { (i & 1) ? half : -half, (i & 2) ? highY : lowY, (i & 4) ? half : -half };
        minX = (std::min)(minX, dot3(p, right));
        maxX = (std::max)(maxX, dot3(p, right));
        minY = (std::min)(minY, dot3(p, up));
        maxY = (std::max)(maxY, dot3(p, up));
        minZ = (std::min)(minZ, dot3(p, forward));
        maxZ = (std::max)(maxZ, dot3(p, forward));
    }
    const float cx = 0.5f * (minX + maxX), cy = 0.5f * (minY + maxY);
    const float hx = 0.5f * (maxX - minX), hy = 0.5f * (maxY - minY);
    const float zr = maxZ - minZ;
    const float texel = 2.0f * (std::max)(hx, hy) / static_cast<float>(size);

    Mat4 basis = Mat4::getIdentity();
    basis.m[0] = right.x;    basis.m[1] = right.y;    basis.m[2] = right.z;
    basis.m[4] = up.x;       basis.m[5] = up.y;       basis.m[6] = up.z;
    basis.m[8] = forward.x;  basis.m[9] = forward.y;  basis.m[10] = forward.z;
    basis.m[12] = right.x * cx + up.x * cy;
    basis.m[13] = right.y * cx + up.y * cy;
    basis.m[14] = right.z * cx + up.z * cy;
    Frustum volume;
    volume.setOrtho(2.0f * hx, 2.0f * hy, minZ, maxZ, basis);

    ShadowCascadeCull cull;
    cull.right[0] = right.x;     cull.right[1] = right.y;     cull.right[2] = right.z;
    cull.up[0] = up.x;           cull.up[1] = up.y;           cull.up[2] = up.z;
    cull.forward[0] = forward.x; cull.forward[1] = forward.y; cull.forward[2] = forward.z;
    const float edges[4][3] = { { 1.0f, 0.0f, maxX }, { -1.0f, 0.0f, -minX },
                                { 0.0f, 1.0f, maxY }, { 0.0f, -1.0f, -minY } };
    std::memcpy(cull.edges, edges, sizeof(edges));
    cull.edgeCount = 4;
    cull.minZ = minZ;
    cull.maxZ = maxZ;

    // The casters at a fixed distance: where one pixel of the camera's projection spans a static
    // texel at the screen's height.
    IRenderView* const view = rendererGetRenderView();
    const float p11 = view->getProjectionMatrixPtr()->m[5];
    const float viewportHeight = static_cast<float>(view->getViewport()[3]);
    const float lodDistance = texel * 0.5f * viewportHeight * (p11 > 0.0f ? p11 : 1.0f);

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    rendererBeginCasterCapture(&volume);
    rendererCasterLodDistance() = lodDistance;
    geom::objectDrawerSubmitShadowCasters(&volume, cull, time, true);
    rendererCasterLodDistance() = 0.0f;
    rendererEndCasterCapture();
    const unsigned int meshCasters = static_cast<unsigned int>(rendererShadowCasters().size());
    terrain->gatherShadowCasters(volume);

    IDirect3DSurface9* depth = 0;
    HRESULT hr = device()->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE,
                                                     0, TRUE, &depth, 0);
    if (hr < 0 || !depth)
    {
        std::fprintf(stderr, "[SUNSTATIC] depth target %ux%u: hr=0x%08lX\n", size, size,
                     static_cast<unsigned long>(hr));
        return false;
    }

    IDirect3DSurface9* savedTarget = stateManagerGetRenderTarget(0);
    IDirect3DSurface9* savedDepth = stateManagerGetDepthStencilSurface();
    if (!savedTarget)
        savedTarget = rendererSceneColorSurface();
    if (!savedDepth)
        savedDepth = rendererDepthStencilSurface();
    if (savedTarget)
        savedTarget->AddRef();
    if (savedDepth)
        savedDepth->AddRef();
    D3DVIEWPORT9 savedViewport;
    device()->GetViewport(&savedViewport);
    device()->SetTexture(kWaterSceneSampler, 0);
    device()->SetTexture(3, 0);

    IDirect3DSurface9* target = 0;
    g_atlas->GetSurfaceLevel(0, &target);
    stateManagerSetRenderTarget(0, target);
    stateManagerSetDepthStencilSurface(depth);
    stateManagerMarkStandardDepth(true);
    flushRenderTargetsSeam();
    const D3DVIEWPORT9 viewport = { 0, 0, size, size, 0.0f, 1.0f };
    device()->SetViewport(&viewport);
    gpuTimerMark("sun static");   // [DEV] BF2_GPU_TIMERS
    stateManagerClear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFFFFFFFFu, 1.0f, 0);

    // World to the static cascade's clip space (row vectors, as the effects multiply).
    const float viewProjection[16] = {
        right.x / hx, up.x / hy, forward.x / zr, 0.0f,
        right.y / hx, up.y / hy, forward.y / zr, 0.0f,
        right.z / hx, up.z / hy, forward.z / zr, 0.0f,
        -cx / hx, -cy / hy, -minZ / zr, 1.0f };
    static const unsigned int key = raHashString(std::string("RaShadowDepth"));
    terrain->drawShadowCasters(viewProjection, cull);
    renderQueuesDrawShadowCasters(key, viewProjection, cull);

    stateManagerSetRenderTarget(0, savedTarget);
    stateManagerSetDepthStencilSurface(savedDepth);
    stateManagerMarkStandardDepth(false);   // the scene depth is reversed-Z again
    flushRenderTargetsSeam();
    device()->SetViewport(&savedViewport);
    if (target)
        target->Release();
    if (savedTarget)
        savedTarget->Release();
    if (savedDepth)
        savedDepth->Release();
    depth->Release();

    // Rows: world position to static texel (centres at n + 0.5; x over 2 hx, y over 2 hy, down) and
    // light depth.
    const float sx = 0.5f * static_cast<float>(size) / hx;
    const float sy = 0.5f * static_cast<float>(size) / hy;
    const float halfSize = 0.5f * static_cast<float>(size);
    float* const c = g_layers;
    c[0] = right.x * sx;  c[1] = right.y * sx;  c[2] = right.z * sx;  c[3] = halfSize - cx * sx;
    c[4] = -up.x * sy;    c[5] = -up.y * sy;    c[6] = -up.z * sy;    c[7] = halfSize + cy * sy;
    c[8] = forward.x / zr; c[9] = forward.y / zr; c[10] = forward.z / zr; c[11] = -minZ / zr;
    c[12] = static_cast<float>(size);
    c[13] = texel;
    // [3].zw: the offsets, set per frame. The slope's depth runs along the light's up axis (the
    // ground recedes along it), whose texels are the finer ones at a low sun.
    g_depthPerTexel = (2.0f * (std::min)(hx, hy) / static_cast<float>(size)) / zr;
    c[22] = 1.0f;                                 // [5].z: drawn

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[SUNSTATIC] static cascade %ux%u over %.0f x %.0f m (texel %.2f m, depth "
                 "%.0f m; map half %.0f m, heights %.1f..%.1f): %u mesh casters at %.0f m, %.1f ms on "
                 "the CPU\n", size, size, 2.0f * hx, 2.0f * hy, texel, zr, half, terrain->m_minHeight,
                 terrain->m_maxHeight, meshCasters, lodDistance, ms);
    return true;
}

// The shadow height into the atlas below the static square, from the far ground's coarse heights;
// fills [5].w, [6] and [7]. False without a far ground.
bool drawShadowHeight(geom::Terrain* terrain, const V3& towardSun)
{
    const float* heights = 0;
    int n = 0;
    float origin = 0.0f, cell = 0.0f;
    unsigned int generation = 0;
    if (!geom::farGroundCoarseHeights(heights, n, origin, cell, generation) || n < 2)
        return false;
    const float horizontal = std::sqrt(towardSun.x * towardSun.x + towardSun.z * towardSun.z);
    if (!(horizontal > 1e-4f) || !(towardSun.y > 0.0f))
        return false;   // the sun overhead or below the horizon: no terrain shadow to march
    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    const unsigned int h = g_atlasHeightSize;
    const float extent = cell * static_cast<float>(n - 1);
    const float hcell = extent / static_cast<float>(h);
    const float half = 0.5f * static_cast<float>(terrain->m_patchCount * terrain->m_patchSize)
                     * terrain->m_primaryWorldScale.x;
    const float dx = towardSun.x / horizontal, dz = towardSun.z / horizontal;
    const float rise = towardSun.y / horizontal;   // metres up per metre toward the sun
    float top = -1e30f, bottom = 1e30f;
    for (int k = 0; k < n * n; ++k)
    {
        top = (std::max)(top, heights[k]);
        bottom = (std::min)(bottom, heights[k]);
    }
    const float none = bottom - 1000.0f;   // nothing toward the sun: well under the lowest ground

    const auto sample = [&](float x, float z) {
        const float u = (std::min)((std::max)((x - origin) / cell, 0.0f), static_cast<float>(n - 1));
        const float v = (std::min)((std::max)((z - origin) / cell, 0.0f), static_cast<float>(n - 1));
        const int i = (std::min)(static_cast<int>(u), n - 2), j = (std::min)(static_cast<int>(v), n - 2);
        const float fu = u - static_cast<float>(i), fv = v - static_cast<float>(j);
        const float* const r0 = heights + static_cast<std::size_t>(j) * n + i;
        const float* const r1 = r0 + n;
        return (r0[0] + (r0[1] - r0[0]) * fu) + ((r1[0] + (r1[1] - r1[0]) * fu)
                - (r0[0] + (r0[1] - r0[0]) * fu)) * fv;
    };
    const auto onMap = [&](float x, float z) {
        return std::fabs(x) < half && std::fabs(z) < half;
    };

    // Per texel: the highest h(s) - rise * s over the march toward the sun, from one coarse cell
    // out (the ground under the texel is not its own caster), until nothing ahead can be higher.
    // The step grows with the distance (a far ridge's shadow is soft anyway); rows on threads.
    const float step = 0.5f * cell;
    const float start = 1.5f * cell;
    const float bias = 1.0f + rise * cell;   // what the coarse cells' own slope gives
    std::vector<float> out(static_cast<std::size_t>(h) * h);
    const auto rows = [&](unsigned int j0, unsigned int j1) {
        for (unsigned int j = j0; j < j1; ++j)
            for (unsigned int i = 0; i < h; ++i)
            {
                const float x = origin + (static_cast<float>(i) + 0.5f) * hcell;
                const float z = origin + (static_cast<float>(j) + 0.5f) * hcell;
                const bool fromMap = onMap(x, z);
                float best = none;
                for (float s = start; ; s += step + 0.01f * s)
                {
                    if (top - rise * s <= best)
                        break;
                    const float px = x + dx * s, pz = z + dz * s;
                    if (px < origin || pz < origin || px > origin + extent || pz > origin + extent)
                        break;
                    if (fromMap && onMap(px, pz))
                        continue;   // the map's own terrain is in the static cascade
                    best = (std::max)(best, sample(px, pz) - rise * s);
                }
                out[static_cast<std::size_t>(j) * h + i] = best - bias;
            }
    };
    {
        const unsigned int threads = (std::max)(1u, (std::min)(8u, std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (unsigned int t = 1; t < threads; ++t)
            pool.emplace_back(rows, h * t / threads, h * (t + 1) / threads);
        rows(0, h / threads);
        for (size_t t = 0; t < pool.size(); ++t)
            pool[t].join();
    }

    // Up through a system-memory copy into the atlas's rows below the static square.
    IDirect3DTexture9* staging = 0;
    HRESULT hr = device()->CreateTexture(h, h, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &staging, 0);
    if (hr < 0 || !staging)
        return false;
    D3DLOCKED_RECT locked;
    if (staging->LockRect(0, &locked, 0, 0) >= 0)
    {
        for (unsigned int j = 0; j < h; ++j)
            std::memcpy(static_cast<unsigned char*>(locked.pBits) + static_cast<std::size_t>(j) * locked.Pitch,
                        &out[static_cast<std::size_t>(j) * h], sizeof(float) * h);
        staging->UnlockRect(0);
    }
    IDirect3DSurface9* src = 0;
    IDirect3DSurface9* dst = 0;
    staging->GetSurfaceLevel(0, &src);
    g_atlas->GetSurfaceLevel(0, &dst);
    const POINT at = { 0, static_cast<LONG>(g_atlasSize) };
    hr = (src && dst) ? device()->UpdateSurface(src, 0, dst, &at) : E_FAIL;
    if (src)
        src->Release();
    if (dst)
        dst->Release();
    staging->Release();
    if (hr < 0)
    {
        std::fprintf(stderr, "[SUNSTATIC] shadow height upload: hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    const float atlasHeight = static_cast<float>(g_atlasSize + h);
    float* const c = g_layers;
    c[23] = 1.0f;                                          // [5].w: drawn
    c[24] = 1.0f / extent;                                 // [6]: world x, z to the square's uv
    c[25] = -origin / extent;
    c[26] = -origin / extent;
    c[27] = (std::max)(2.0f, rise * hcell);                // the soft band (m)
    c[28] = static_cast<float>(h) / static_cast<float>(g_atlasSize);   // [7]: to atlas uv
    c[29] = static_cast<float>(h) / atlasHeight;
    c[30] = 0.0f;
    c[31] = static_cast<float>(g_atlasSize) / atlasHeight;

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "[SUNSTATIC] shadow height %ux%u over %.0f m (texel %.0f m, sun rise %.3f): "
                 "%.1f ms\n", h, h, extent, hcell, rise, ms);
    return true;
}

// [TOOLS] An R32F region of the atlas to an 8-bit grey PNG `side` pixels wide (point-sampled), each
// value mapped from [lo, hi]; the range it held in `outLo`, `outHi`.
bool saveRegion(const float* texels, unsigned int pitchFloats, unsigned int x0, unsigned int y0,
                unsigned int w, unsigned int hgt, unsigned int side, bool autoRange,
                const std::string& path, float& outLo, float& outHi)
{
    float lo = 1e30f, hi = -1e30f;
    for (unsigned int y = 0; y < hgt; ++y)
        for (unsigned int x = 0; x < w; ++x)
        {
            const float v = texels[static_cast<std::size_t>(y0 + y) * pitchFloats + x0 + x];
            lo = (std::min)(lo, v);
            hi = (std::max)(hi, v);
        }
    outLo = lo;
    outHi = hi;
    if (!autoRange)
    {
        lo = 0.0f;
        hi = 1.0f;
    }
    const unsigned int sideY = static_cast<unsigned int>(
        static_cast<float>(side) * static_cast<float>(hgt) / static_cast<float>(w));
    IDirect3DSurface9* image = 0;
    if (device()->CreateOffscreenPlainSurface(side, sideY, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &image, 0) < 0)
        return false;
    D3DLOCKED_RECT locked;
    if (image->LockRect(&locked, 0, 0) >= 0)
    {
        for (unsigned int y = 0; y < sideY; ++y)
            for (unsigned int x = 0; x < side; ++x)
            {
                const unsigned int sx = x * w / side, sy = y * hgt / sideY;
                const float v = texels[static_cast<std::size_t>(y0 + sy) * pitchFloats + x0 + sx];
                const float t = hi > lo ? (v - lo) / (hi - lo) : 0.0f;
                const unsigned int g = static_cast<unsigned int>((std::min)((std::max)(t, 0.0f), 1.0f) * 255.0f);
                reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(locked.pBits)
                    + static_cast<std::size_t>(y) * locked.Pitch)[x] = 0xFF000000u | (g << 16) | (g << 8) | g;
            }
        image->UnlockRect();
    }
    const bool ok = D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, image, 0, 0) >= 0;
    image->Release();
    return ok;
}

std::string g_toolsMessage;

} // namespace

// [TOOLS] tools.sun (render milestone 14): the static layers' state to base.txt, the static cascade
// (light depth, 1 white = no caster) to base-static.png and the shadow height to base-height.png.
bool toolsWriteSun(const char* base, const char*& message)
{
    message = "";
    if (!base || !g_rendererBase || !g_atlas)
    {
        g_toolsMessage = "the static layers are not drawn";
        message = g_toolsMessage.c_str();
        return false;
    }
    const std::string root(base);
    FILE* const f = std::fopen((root + ".txt").c_str(), "w");
    if (!f)
    {
        g_toolsMessage = "cannot write " + root + ".txt";
        message = g_toolsMessage.c_str();
        return false;
    }
    std::fprintf(f, "# tools.sun (render milestone 14): the sun's static layers as drawn\n");
    std::fprintf(f, "[settings] static %u, height %u, normal offset %.2f texels, depth "
                    "bias %.2f texels\n", g_atlasSize, g_atlasHeightSize,
                 g_settings.normalOffset, g_settings.depthBias);
    std::fprintf(f, "[sun] travels %.4f %.4f %.4f\n", g_drawn.sun[0], g_drawn.sun[1], g_drawn.sun[2]);
    for (unsigned int i = 0; i < kSunStaticVec4s; ++i)
        std::fprintf(f, "[RaSunStatic %u] %.6g %.6g %.6g %.6g\n", i, g_layers[4 * i], g_layers[4 * i + 1],
                     g_layers[4 * i + 2], g_layers[4 * i + 3]);

    const unsigned int w = g_atlasSize, hgt = g_atlasSize + g_atlasHeightSize;
    IDirect3DSurface9* copy = 0;
    IDirect3DSurface9* src = 0;
    bool ok = device()->CreateOffscreenPlainSurface(w, hgt, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &copy, 0) >= 0
           && g_atlas->GetSurfaceLevel(0, &src) >= 0
           && device()->GetRenderTargetData(src, copy) >= 0;
    if (src)
        src->Release();
    D3DLOCKED_RECT locked;
    if (ok && copy->LockRect(&locked, 0, D3DLOCK_READONLY) >= 0)
    {
        const float* const texels = static_cast<const float*>(locked.pBits);
        const unsigned int pitch = static_cast<unsigned int>(locked.Pitch) / sizeof(float);
        float lo = 0.0f, hi = 0.0f;
        std::size_t covered = 0;
        for (unsigned int y = 0; y < w; ++y)
            for (unsigned int x = 0; x < w; ++x)
                covered += texels[static_cast<std::size_t>(y) * pitch + x] < 1.0f ? 1u : 0u;
        ok = saveRegion(texels, pitch, 0, 0, w, w, 1024, false, root + "-static.png", lo, hi);
        std::fprintf(f, "[static] light depth %.4f..%.4f, %.1f %% of texels hold a caster\n", lo, hi,
                     100.0 * static_cast<double>(covered) / (static_cast<double>(w) * w));
        ok = saveRegion(texels, pitch, 0, w, g_atlasHeightSize, g_atlasHeightSize, 512, true,
                        root + "-height.png", lo, hi) && ok;
        std::fprintf(f, "[height] %.1f..%.1f m\n", lo, hi);
        copy->UnlockRect();
    }
    if (copy)
        copy->Release();
    std::fclose(f);
    if (!ok)
    {
        g_toolsMessage = "could not read the atlas back";
        message = g_toolsMessage.c_str();
    }
    return ok;
}

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
    g_invalid = true;
}

void rendererSunStaticCastersChanged()
{
    g_castersChanged = true;
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
    g_atlasSize = 0;
    g_atlasHeightSize = 0;
    g_invalid = true;
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

void rendererSunStaticFrame(float* constants, float time)
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
    const V3 forward = { d.x / length, d.y / length, d.z / length };
    const V3 towardSun = { -forward.x, -forward.y, -forward.z };

    geom::Terrain* const terrain = static_cast<geom::Terrain*>(geom::g_Terrain);
    const float* heights = 0;
    int n = 0;
    float origin = 0.0f, cell = 0.0f;
    unsigned int generation = 0;
    geom::farGroundCoarseHeights(heights, n, origin, cell, generation);
    Key now;
    std::memset(&now, 0, sizeof(now));   // compared bytewise, padding included
    now.terrain = terrain;
    now.sun[0] = forward.x;
    now.sun[1] = forward.y;
    now.sun[2] = forward.z;
    now.resolution = g_settings.resolution;
    now.heightSize = g_settings.heightSize;
    now.heights = heights;
    now.heightGeneration = generation;
    const bool changed = g_invalid || std::memcmp(&now, &g_drawn, sizeof(Key)) != 0;
    const std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
    if (changed && terrain && ensureAtlas())
    {
        std::memset(g_layers, 0, sizeof(g_layers));
        g_drawn = now;
        g_invalid = false;
        g_castersChanged = false;
        g_staticDrawn = clock;
        if (forward.y < -0.01f)   // the sun above the horizon: anything to cast
            drawStaticCascade(terrain, forward, time);
        drawShadowHeight(terrain, towardSun);
    }
    else if (g_castersChanged && terrain && g_atlas && forward.y < -0.01f
             && clock - g_staticDrawn > std::chrono::seconds(1))
    {
        // A static caster changed: the static cascade alone, its rows rewritten in place
        g_castersChanged = false;
        g_staticDrawn = clock;
        std::memset(g_layers, 0, 16 * sizeof(float));
        g_layers[22] = 0.0f;
        drawStaticCascade(terrain, forward, time);
    }

    std::memcpy(constants, g_layers, sizeof(g_layers));
    // [3].zw: the receiver offsets, live (renderer.sunStaticNormalOffset, sunStaticDepthBias)
    constants[14] = g_settings.normalOffset;
    constants[15] = g_settings.depthBias * g_depthPerTexel;
    // [4]: toward the sun; w 1: the run-time sun term in force (zero outside the main world pass)
    constants[16] = towardSun.x;
    constants[17] = towardSun.y;
    constants[18] = towardSun.z;
    constants[19] = 1.0f;
    // [5].xy: the atlas's texel in uv
    if (g_atlas)
    {
        constants[20] = 1.0f / static_cast<float>(g_atlasSize);
        constants[21] = 1.0f / static_cast<float>(g_atlasSize + g_atlasHeightSize);
    }
    else
    {
        constants[22] = 0.0f;
        constants[23] = 0.0f;
    }
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

// [TOOLS] tools.atmosphere (docs/eon64/tools.md#atmosphere), renderer side. Not part of retail BF2.
// Render milestone 11's atmosphere for an agent or a person to read without the debug menu: the fit
// and every table's statistics and samples as text, and each table as a PNG scaled to its own
// maximum (linear, no gamma). Runs at the console call, between frames: the managed tables are
// locked, the atlas read back from its render target.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "ToolsAtmosphere.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

float halfToFloat(unsigned short h)
{
    const unsigned int sign = (h >> 15) & 1u, exponent = (h >> 10) & 31u, mantissa = h & 1023u;
    float v;
    if (exponent == 0)
        v = mantissa * (1.0f / 16777216.0f);                         // 2^-24
    else if (exponent == 31)
        v = mantissa ? 0.0f : 65504.0f;                              // NaN as 0, inf as max
    else
    {
        unsigned int bits = ((exponent + 112u) << 23) | (mantissa << 13);
        std::memcpy(&v, &bits, sizeof(v));
    }
    return sign ? -v : v;
}

struct Table
{
    unsigned int w, h;
    std::vector<float> rgba;   // w * h * 4
    const float* at(unsigned int x, unsigned int y) const { return &rgba[(static_cast<size_t>(y) * w + x) * 4]; }
};

bool readTable(IDirect3DTexture9* texture, Table& out, std::string& why)
{
    if (!texture)
    {
        why = "not drawn";
        return false;
    }
    D3DSURFACE_DESC desc;
    IDirect3DSurface9* source = 0;
    IDirect3DSurface9* copy = 0;
    if (texture->GetLevelDesc(0, &desc) < 0 || texture->GetSurfaceLevel(0, &source) < 0)
    {
        why = "no surface";
        return false;
    }
    // Render targets are copied to system memory; managed tables are locked where they are.
    bool ok = desc.Pool == D3DPOOL_MANAGED;
    if (ok)
    {
        copy = source;
        copy->AddRef();
    }
    else
        ok = device()->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                   D3DPOOL_SYSTEMMEM, &copy, 0) >= 0
          && device()->GetRenderTargetData(source, copy) >= 0;
    D3DLOCKED_RECT lr;
    if (ok && copy->LockRect(&lr, 0, D3DLOCK_READONLY) >= 0)
    {
        out.w = desc.Width;
        out.h = desc.Height;
        out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0.0f);
        for (unsigned int y = 0; y < out.h; ++y)
        {
            const unsigned char* const row = static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch;
            for (unsigned int x = 0; x < out.w; ++x)
                for (int k = 0; k < 4; ++k)
                {
                    float v;   // D3D's ABGR formats store R first
                    if (desc.Format == D3DFMT_A32B32G32R32F)
                        std::memcpy(&v, row + x * 16 + k * 4, sizeof(v));
                    else
                    {
                        unsigned short h;
                        std::memcpy(&h, row + x * 8 + k * 2, sizeof(h));
                        v = halfToFloat(h);
                    }
                    out.rgba[(static_cast<size_t>(y) * out.w + x) * 4 + k] = v;
                }
        }
        copy->UnlockRect();
    }
    else
    {
        why = "read back failed";
        ok = false;
    }
    if (copy)
        copy->Release();
    source->Release();
    return ok;
}

// rgb (or alpha alone) scaled so its maximum is 255.
bool writePng(const Table& t, const std::string& path, bool alpha, float& scale)
{
    float peak = 0.0f;
    for (size_t i = 0; i < t.rgba.size(); i += 4)
        for (int k = alpha ? 3 : 0; k < (alpha ? 4 : 3); ++k)
            peak = std::max(peak, t.rgba[i + k]);
    scale = peak > 0.0f ? 255.0f / peak : 0.0f;
    IDirect3DSurface9* surface = 0;
    if (device()->CreateOffscreenPlainSurface(t.w, t.h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, 0) < 0)
        return false;
    D3DLOCKED_RECT lr;
    if (surface->LockRect(&lr, 0, 0) < 0)
    {
        surface->Release();
        return false;
    }
    for (unsigned int y = 0; y < t.h; ++y)
        for (unsigned int x = 0; x < t.w; ++x)
        {
            const float* const p = t.at(x, y);
            unsigned char c[3];
            for (int k = 0; k < 3; ++k)
            {
                const float v = (alpha ? p[3] : p[k]) * scale;
                c[k] = static_cast<unsigned char>(std::min(std::max(v + 0.5f, 0.0f), 255.0f));
            }
            unsigned char* const d = static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch + x * 4;
            d[0] = c[2]; d[1] = c[1]; d[2] = c[0]; d[3] = 255;
        }
    surface->UnlockRect();
    const bool ok = D3DXSaveSurfaceToFileA(path.c_str(), D3DXIFF_PNG, surface, 0, 0) >= 0;
    surface->Release();
    return ok;
}

void writeStats(FILE* f, const char* name, const Table& t)
{
    float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
    double sum[4] = {};
    unsigned int bad = 0;
    for (size_t i = 0; i < t.rgba.size(); i += 4)
        for (int k = 0; k < 4; ++k)
        {
            const float v = t.rgba[i + k];
            if (!(v == v) || v > 60000.0f)
                ++bad;
            lo[k] = std::min(lo[k], v);
            hi[k] = std::max(hi[k], v);
            sum[k] += v;
        }
    const double n = static_cast<double>(t.w) * t.h;
    std::fprintf(f, "[%s] %ux%u; per channel r g b a: min %.4g %.4g %.4g %.4g, max %.4g %.4g %.4g %.4g, "
                    "mean %.4g %.4g %.4g %.4g; non-finite %u\n",
                 name, t.w, t.h, lo[0], lo[1], lo[2], lo[3], hi[0], hi[1], hi[2], hi[3],
                 sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n, bad);
}

void writeRgb(FILE* f, const float* p, bool alpha)
{
    if (alpha)
        std::fprintf(f, " %.4f %.4f %.4f|%.3f", p[0], p[1], p[2], p[3]);
    else
        std::fprintf(f, " %.4f %.4f %.4f", p[0], p[1], p[2]);
}

// Samples on a grid: `cols` columns across each listed row.
void writeGrid(FILE* f, const char* name, const Table& t, const unsigned int* rows, unsigned int rowCount,
               unsigned int cols, const char* rowMeaning, const char* colMeaning)
{
    std::fprintf(f, "[%s] samples: rows %s; columns %s at x =", name, rowMeaning, colMeaning);
    for (unsigned int c = 0; c < cols; ++c)
        std::fprintf(f, " %u", c * (t.w - 1) / (cols - 1));
    std::fprintf(f, "\n");
    for (unsigned int r = 0; r < rowCount; ++r)
    {
        const unsigned int y = std::min(rows[r], t.h - 1);
        std::fprintf(f, "[%s]  y %3u:", name, y);
        for (unsigned int c = 0; c < cols; ++c)
            writeRgb(f, t.at(c * (t.w - 1) / (cols - 1), y), false);
        std::fprintf(f, "\n");
    }
}

std::string g_message;

} // anonymous namespace

bool toolsWriteAtmosphere(const char* base, const char*& message)
{
    message = "";
    const AtmosphereInfo a = rendererAtmosphereInfo();
    if (!base || !device())
        g_message = "no renderer";
    else if (!rendererAtmosphereEnabled())
        g_message = "renderer.atmosphere is 0";
    else if (!a.fitted)
        g_message = "no fit yet (a level with a sky dome and fog has not drawn)";
    else
        g_message.clear();
    if (!g_message.empty())
    {
        message = g_message.c_str();
        return false;
    }

    const std::string root(base);
    FILE* const f = std::fopen((root + ".txt").c_str(), "w");
    if (!f)
    {
        g_message = "cannot write " + root + ".txt";
        message = g_message.c_str();
        return false;
    }
    std::fprintf(f, "# tools.atmosphere (render milestone 11): the fit and its tables as drawn\n");
    std::fprintf(f, "[fit] %.0f ms; reference height %.1f m; fog end %.0f m\n", a.fitMs, a.refHeight, a.fogEnd);
    std::fprintf(f, "[fit] rayleigh tint x Earth %.4f %.4f %.4f\n", a.rayleigh[0], a.rayleigh[1], a.rayleigh[2]);
    std::fprintf(f, "[fit] sky gain %.4f, applied %.4f (renderer.skyGainBlend %.2f)\n", a.skyGain,
                 a.skyGainApplied, rendererSkyGainBlend());
    std::fprintf(f, "[fit] sun above the atmosphere %.4f %.4f %.4f (irradiance)\n", a.sunTop[0], a.sunTop[1], a.sunTop[2]);
    std::fprintf(f, "[fit] zenith target %.4f %.4f %.4f%s, model %.4f %.4f %.4f (before the gain)\n",
                 a.zenithTarget[0], a.zenithTarget[1], a.zenithTarget[2], !a.haveZenith ? " (unread)" : a.zenithClear ? " (clear texels)" : " (overcast, whole band)",
                 a.zenithModel[0], a.zenithModel[1], a.zenithModel[2]);
    std::fprintf(f, "[fit] horizon target %.4f %.4f %.4f, model %.4f %.4f %.4f (before the gain)\n",
                 a.horizonTarget[0], a.horizonTarget[1], a.horizonTarget[2], a.horizonModel[0],
                 a.horizonModel[1], a.horizonModel[2]);
    std::fprintf(f, "[fit] ground albedo %.4f %.4f %.4f\n", a.groundAlbedo[0], a.groundAlbedo[1], a.groundAlbedo[2]);
    std::fprintf(f, "[fit] haze density %.4e /m\n", a.hazeDensity);
    std::fprintf(f, "[atlas] format %s, vertex fetch %s, filtered %s\n",
                 a.atlasFloat32 ? "A32B32G32R32F" : "A16B16G16R16F", a.atlasVertexFetch ? "yes" : "no",
                 a.atlasVertexFilter ? "yes" : "no");

    struct Entry
    {
        const char* name;
        IDirect3DTexture9* texture;
    };
    const Entry entries[4] = { { "transmittance", a.transmittance }, { "multiscatter", a.multiScatter },
                               { "skyview", a.skyView }, { "perspective", a.perspective } };
    std::string failed;
    for (int i = 0; i < 4; ++i)
    {
        Table t;
        std::string why;
        if (!readTable(entries[i].texture, t, why))
        {
            std::fprintf(f, "[%s] not read: %s\n", entries[i].name, why.c_str());
            failed += std::string(failed.empty() ? "" : ", ") + entries[i].name;
            continue;
        }
        writeStats(f, entries[i].name, t);
        float scale = 0.0f;
        const std::string png = root + "-" + entries[i].name + ".png";
        if (writePng(t, png, false, scale))
            std::fprintf(f, "[%s] png %s (rgb x %.4g)\n", entries[i].name, png.c_str(), scale);
        if (i == 0)
        {
            const unsigned int rows[5] = { 0, 8, 16, 32, 63 };
            writeGrid(f, entries[i].name, t, rows, 5, 9, "altitude (0 ground, 63 top)",
                      "view (0 straight up, 255 the horizon)");
        }
        else if (i == 1)
        {
            const unsigned int rows[5] = { 0, 4, 8, 16, 31 };
            writeGrid(f, entries[i].name, t, rows, 5, 9, "altitude (0 ground, 31 top)",
                      "sun cosine (0 = -1, 31 = +1)");
        }
        else if (i == 2)
        {
            const unsigned int rows[9] = { 0, 13, 27, 40, 50, 53, 56, 70, 107 };
            writeGrid(f, entries[i].name, t, rows, 9, 7, "zenith (0) to horizon (53-54) to nadir (107)",
                      "azimuth (0 toward the sun, 191 away)");
        }
        else
        {
            const std::string alphaPng = root + "-perspective-transmittance.png";
            float alphaScale = 0.0f;
            if (writePng(t, alphaPng, true, alphaScale))
                std::fprintf(f, "[perspective] png %s (transmittance x %.4g)\n", alphaPng.c_str(), alphaScale);
            // Per slice, at four tiles: the screen centre, the top centre, and the middle row's left
            // and right edges. rgb in-scatter | transmittance.
            static const unsigned int kTiles[4][2] = { { 16, 16 }, { 16, 2 }, { 0, 16 }, { 31, 16 } };
            static const char* const kTileNames[4] = { "centre", "top", "left", "right" };
            std::fprintf(f, "[perspective] per slice (distance = fog end * ((slice + 1) / 32)^2): "
                            "in-scatter rgb | transmittance\n");
            for (unsigned int s = 0; s < 32; ++s)
            {
                const float d = a.fogEnd * ((s + 1) / 32.0f) * ((s + 1) / 32.0f);
                std::fprintf(f, "[perspective]  slice %2u %6.1f m:", s, d);
                for (int k = 0; k < 4; ++k)
                {
                    std::fprintf(f, "  %s", kTileNames[k]);
                    writeRgb(f, t.at(s * 32 + kTiles[k][0], kTiles[k][1]), true);
                }
                std::fprintf(f, "\n");
            }
        }
    }
    std::fclose(f);
    g_message = failed.empty() ? root + ".txt" : root + ".txt (not read: " + failed + ")";
    message = g_message.c_str();
    return failed.empty();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

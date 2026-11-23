// [TOOLS] tools.atmosphere (docs/lanes/tools.md#atmosphere), renderer side. Not part of retail BF2.
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
#include <cmath>
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
                    float v;   // D3D's ABGR formats store R first, ARGB ones B first
                    if (desc.Format == D3DFMT_A8R8G8B8 || desc.Format == D3DFMT_X8R8G8B8)
                        v = row[x * 4 + (k == 3 ? 3 : 2 - k)] * (1.0f / 255.0f);
                    else if (desc.Format == D3DFMT_A32B32G32R32F)
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


// One face and mip of a managed A16B16G16R16F cube, as a table.
bool readCubeFace(IDirect3DCubeTexture9* cube, int face, unsigned int level, Table& out)
{
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    if (!cube || cube->GetLevelDesc(level, &desc) < 0
        || cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lr, 0, D3DLOCK_READONLY) < 0)
        return false;
    out.w = desc.Width;
    out.h = desc.Height;
    out.rgba.assign(static_cast<size_t>(out.w) * out.h * 4, 0.0f);
    for (unsigned int y = 0; y < out.h; ++y)
    {
        const unsigned short* const row = reinterpret_cast<const unsigned short*>(
            static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch);
        for (unsigned int x = 0; x < out.w * 4; ++x)
            out.rgba[static_cast<size_t>(y) * out.w * 4 + x] = halfToFloat(row[x]);
    }
    cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
    return true;
}

// D3D's cube layout, as SkyCube.cpp.
void cubeDirection(int face, unsigned int x, unsigned int y, unsigned int size, float d[3])
{
    const float u = 2.0f * (x + 0.5f) / size - 1.0f, v = 2.0f * (y + 0.5f) / size - 1.0f;
    const float dirs[6][3] = { { 1.0f, -v, -u }, { -1.0f, -v, u }, { u, 1.0f, v },
                               { u, -1.0f, -v }, { u, -v, 1.0f }, { -u, -v, -1.0f } };
    const float l = std::sqrt(dirs[face][0] * dirs[face][0] + dirs[face][1] * dirs[face][1]
                              + dirs[face][2] * dirs[face][2]);
    for (int k = 0; k < 3; ++k)
        d[k] = dirs[face][k] / l;
}

float cubeSolidAngle(unsigned int x, unsigned int y, unsigned int size)
{
    struct Area
    {
        static float at(float u, float v) { return std::atan2(u * v, std::sqrt(u * u + v * v + 1.0f)); }
    };
    const float inv = 1.0f / size;
    const float u0 = 2.0f * x * inv - 1.0f, u1 = u0 + 2.0f * inv;
    const float v0 = 2.0f * y * inv - 1.0f, v1 = v0 + 2.0f * inv;
    return Area::at(u0, v0) - Area::at(u0, v1) - Area::at(u1, v0) + Area::at(u1, v1);
}

// RaSkySH at n, as the shaders evaluate it (contract 10).
void evalSkySh(const float sh[7][4], const float n[3], float out[3])
{
    const float a[4] = { n[0], n[1], n[2], 1.0f };
    const float b[4] = { n[0] * n[1], n[1] * n[2], n[2] * n[2], n[2] * n[0] };
    for (int ch = 0; ch < 3; ++ch)
    {
        float v = sh[6][ch] * (n[0] * n[0] - n[1] * n[1]);
        for (int k = 0; k < 4; ++k)
            v += sh[ch][k] * a[k] + sh[3 + ch][k] * b[k];
        out[ch] = v;
    }
}

// The sky cube (task 7): per mip its faces' statistics, one PNG (faces across, mips down, scaled to
// the top mip's maximum), the irradiance check against a brute-force cosine integral of the top
// mip, and the BRDF table.
bool writeSkyCube(FILE* f, const std::string& root)
{
    const SkyCubeInfo c = rendererSkyCubeInfo();
    std::fprintf(f, "[skycube] %s; built in %.0f ms; %u per face, %u mips (mip m roughness m / %u); "
                    "irradiance up %.4f %.4f %.4f\n",
                 c.live ? "live" : "NOT LIVE", c.buildMs, c.cubeSize, c.cubeMips, c.cubeMips - 1,
                 c.irradianceUp[0], c.irradianceUp[1], c.irradianceUp[2]);
    for (int r = 0; r < 7; ++r)
        std::fprintf(f, "[skycube] RaSkySH[%d] %.5f %.5f %.5f %.5f\n", r, c.sh[r][0], c.sh[r][1], c.sh[r][2], c.sh[r][3]);
    if (!c.live || !c.cube)
        return false;

    static const char* const kFaces[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };
    std::vector<Table> top(6);
    Table strip;
    strip.w = c.cubeSize * 6;
    strip.h = c.cubeSize * c.cubeMips;
    strip.rgba.assign(static_cast<size_t>(strip.w) * strip.h * 4, 0.0f);
    bool ok = true;
    for (unsigned int level = 0; level < c.cubeMips; ++level)
    {
        std::fprintf(f, "[skycube] mip %u, face means:", level);
        for (int face = 0; face < 6; ++face)
        {
            Table t;
            if (!readCubeFace(c.cube, face, level, t))
            {
                std::fprintf(f, " %s not read", kFaces[face]);
                ok = false;
                continue;
            }
            double sum[3] = {};
            unsigned int bad = 0;
            for (size_t i = 0; i < t.rgba.size(); i += 4)
                for (int k = 0; k < 3; ++k)
                {
                    const float v = t.rgba[i + k];
                    if (!(v == v) || v > 60000.0f || v < 0.0f)
                        ++bad;
                    sum[k] += v;
                }
            const double n = static_cast<double>(t.w) * t.h;
            std::fprintf(f, "  %s %.4f %.4f %.4f", kFaces[face], sum[0] / n, sum[1] / n, sum[2] / n);
            if (bad)
                std::fprintf(f, " (%u bad)", bad);
            const unsigned int scale = c.cubeSize / t.w;
            for (unsigned int y = 0; y < c.cubeSize; ++y)
                for (unsigned int x = 0; x < c.cubeSize; ++x)
                    std::memcpy(&strip.rgba[((static_cast<size_t>(level) * c.cubeSize + y) * strip.w
                                             + face * c.cubeSize + x) * 4],
                                t.at(x / scale, y / scale), sizeof(float) * 4);
            if (level == 0)
                top[face] = t;
        }
        std::fprintf(f, "\n");
    }
    float scale = 0.0f;
    const std::string png = root + "-skycube.png";
    if (writePng(strip, png, false, scale))
        std::fprintf(f, "[skycube] png %s (faces +x -x +y -y +z -z across, mips 0-%u down; rgb x %.4g)\n",
                     png.c_str(), c.cubeMips - 1, scale);

    // Irradiance / pi: SH against the cosine integral of the top mip, times the same scale.
    if (ok)
    {
        static const float kDirs[8][3] = { { 0, 1, 0 }, { 0, -1, 0 }, { 1, 0, 0 }, { -1, 0, 0 },
                                           { 0, 0, 1 }, { 0, 0, -1 }, { 0.7071f, 0.7071f, 0 },
                                           { 0, -0.7071f, 0.7071f } };
        static const char* const kNames[8] = { "up", "down", "+x", "-x", "+z", "-z", "+x up 45", "+z down 45" };
        std::fprintf(f, "[skycube] irradiance / pi: SH | cube integral (scaled alike) | ratio\n");
        for (int i = 0; i < 8; ++i)
        {
            double e[3] = {};
            for (int face = 0; face < 6; ++face)
                for (unsigned int y = 0; y < c.cubeSize; ++y)
                    for (unsigned int x = 0; x < c.cubeSize; ++x)
                    {
                        float d[3];
                        cubeDirection(face, x, y, c.cubeSize, d);
                        const float cosine = d[0] * kDirs[i][0] + d[1] * kDirs[i][1] + d[2] * kDirs[i][2];
                        if (cosine <= 0.0f)
                            continue;
                        const float w = cosine * cubeSolidAngle(x, y, c.cubeSize);
                        const float* const p = top[face].at(x, y);
                        for (int k = 0; k < 3; ++k)
                            e[k] += p[k] * w;
                    }
            float sh[3];
            evalSkySh(c.sh, kDirs[i], sh);
            std::fprintf(f, "[skycube]  %-10s %.4f %.4f %.4f | %.4f %.4f %.4f | %.3f %.3f %.3f\n", kNames[i],
                         sh[0], sh[1], sh[2], e[0] / 3.14159265, e[1] / 3.14159265, e[2] / 3.14159265,
                         sh[0] / std::max(e[0] / 3.14159265, 1e-9), sh[1] / std::max(e[1] / 3.14159265, 1e-9),
                         sh[2] / std::max(e[2] / 3.14159265, 1e-9));
        }
    }

    Table brdf;
    std::string why;
    if (!readTable(c.brdf, brdf, why))
    {
        std::fprintf(f, "[brdf] not read: %s\n", why.c_str());
        return false;
    }
    writeStats(f, "brdf", brdf);
    const std::string brdfPng = root + "-brdf.png";
    if (writePng(brdf, brdfPng, false, scale))
        std::fprintf(f, "[brdf] png %s (rgb x %.4g)\n", brdfPng.c_str(), scale);
    const unsigned int rows[5] = { 0, 16, 32, 48, 63 };
    writeGrid(f, "brdf", brdf, rows, 5, 5, "roughness (y + 0.5) / 64", "n.v (x + 0.5) / 64; rgb = scale, bias, 0");
    return ok;
}

std::string g_message;

} // anonymous namespace

bool toolsWriteAtmosphere(const char* base, const char*& message)
{
    message = "";
    const AtmosphereInfo a = rendererAtmosphereInfo();
    if (!base || !device())
        g_message = "no renderer";
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
    std::fprintf(f, "# tables built on a worker; the live build may lag the settings by a frame or two\n");
    std::fprintf(f, "[fit] %.0f ms (camera sky view %.0f ms); reference height %.1f m; fog end %.0f m\n", a.fitMs,
                 a.bandMs, a.refHeight, a.fogEnd);
    std::fprintf(f, "[weather] visibility %.0f m, aerosol %d (%s), mixing %.0f m, altitude %s\n",
                 a.weather.visibility, a.weather.aerosol, a.aerosolName, a.weather.mixing,
                 a.weather.altitude < -1000.0f ? "auto" : "set");
    std::fprintf(f, "[weather] file %s\n", rendererWeatherSource());
    std::fprintf(f, "[air] ground %.1f m above sea level (%s); world y 0 at %.1f m; mixing layer to %.1f m\n",
                 a.groundAltitude, a.altitudeSource, a.seaOffset, a.mixingTop);
    std::fprintf(f, "[air] aerosol extinction %.5f %.5f %.5f /km, scattering %.5f %.5f %.5f /km, g %.2f\n",
                 a.aerosolExtinction[0], a.aerosolExtinction[1], a.aerosolExtinction[2], a.aerosolScattering[0],
                 a.aerosolScattering[1], a.aerosolScattering[2], a.aerosolG);
    std::fprintf(f, "[air] camera sky view at %.0f m above sea level\n", a.skyViewAltitude);
    std::fprintf(f, "[sun] surfaces' factor %.4f %.4f %.4f (this air over the reference's)\n", a.sunFactor[0],
                 a.sunFactor[1], a.sunFactor[2]);
    std::fprintf(f, "[fit] authored horizon / model %.4f (not applied)\n", a.skyGain);
    std::fprintf(f, "[fit] sun above the atmosphere %.4f %.4f %.4f (irradiance)\n", a.sunTop[0], a.sunTop[1], a.sunTop[2]);
    std::fprintf(f, "[fit] zenith model %.4f %.4f %.4f\n",
                 a.zenithModel[0], a.zenithModel[1], a.zenithModel[2]);
    std::fprintf(f, "[fit] horizon target %.4f %.4f %.4f, model %.4f %.4f %.4f\n",
                 a.horizonTarget[0], a.horizonTarget[1], a.horizonTarget[2], a.horizonModel[0],
                 a.horizonModel[1], a.horizonModel[2]);
    std::fprintf(f, "[fit] ground albedo %.4f %.4f %.4f\n", a.groundAlbedo[0], a.groundAlbedo[1], a.groundAlbedo[2]);
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
            // Contract 27: in-scatter slices (rgb over the sky light toward the tile's ray), then
            // transmittance slices (rgb), then the sky block. Per slice, at four tiles: the screen
            // centre, the top centre, and the middle row's left and right edges.
            static const unsigned int kTiles[4][2] = { { 16, 16 }, { 16, 2 }, { 0, 16 }, { 31, 16 } };
            static const char* const kTileNames[4] = { "centre", "top", "left", "right" };
            std::fprintf(f, "[perspective] per slice (distance = 40 km * ((slice + 1) / 32)^3): "
                            "in-scatter / sky light rgb | transmittance rgb\n");
            for (unsigned int s = 0; s < 32 && t.w >= 65 * 32; ++s)
            {
                const float u = (s + 1) / 32.0f;
                std::fprintf(f, "[perspective]  slice %2u %8.1f m:", s, 40000.0f * u * u * u);
                for (int k = 0; k < 4; ++k)
                {
                    std::fprintf(f, "  %s", kTileNames[k]);
                    writeRgb(f, t.at(s * 32 + kTiles[k][0], kTiles[k][1]), false);
                    std::fprintf(f, " |");
                    writeRgb(f, t.at((32 + s) * 32 + kTiles[k][0], kTiles[k][1]), false);
                }
                std::fprintf(f, "\n");
            }
            std::fprintf(f, "[perspective] sky block, column 16, rows 0 (horizon), 4, 8, 16, 31 (zenith):");
            for (unsigned int r = 0; r < 32 && t.w >= 65 * 32; r = r == 16 ? 31 : r == 0 ? 4 : r * 2)
                writeRgb(f, t.at(64 * 32 + 16, r), false);
            std::fprintf(f, "\n");
        }
    }
    // Render milestone 12: the clouds' light (Atmosphere.cpp, buildCloudLight).
    {
        Table t;
        std::string why;
        if (!readTable(a.cloudLight, t, why) || t.w != 64 || t.h != 32)
        {
            std::fprintf(f, "[cloudlight] not read: %s\n", why.empty() ? "unexpected size" : why.c_str());
            failed += std::string(failed.empty() ? "" : ", ") + "cloudlight";
        }
        else
        {
            writeStats(f, "cloudlight", t);
            float scale = 0.0f;
            const std::string png = root + "-cloudlight.png";
            if (writePng(t, png, false, scale))
                std::fprintf(f, "[cloudlight] png %s (rgb x %.4g)\n", png.c_str(), scale);
            // Per altitude, at the level's origin (texel 3 of 8, -35 km) and the tiles' x edges
            // (-245 and +245 km): the sun's irradiance, the sky from above, the ground from below.
            static const char* const kRows[4] = { "sun", "sky", "ground", "toward-sun" };
            std::fprintf(f, "[cloudlight] per altitude km: at x = -245, -35, +245 km (z -35 km), "
                            "rgb per row sun | sky | ground | sky toward the sun\n");
            for (unsigned int l = 0; l < 8; ++l)
            {
                std::fprintf(f, "[cloudlight]  %5.2f:", 12.0 * (l + 0.5) / 8.0);
                for (unsigned int row = 0; row < 4; ++row)
                {
                    std::fprintf(f, "  %s", kRows[row]);
                    static const unsigned int kX[3] = { 0, 3, 7 };
                    for (int k = 0; k < 3; ++k)
                        writeRgb(f, t.at(l * 8 + kX[k], row * 8 + 3), false);
                }
                std::fprintf(f, "\n");
            }
        }
    }
    if (!writeSkyCube(f, root))
        failed += std::string(failed.empty() ? "" : ", ") + "skycube";
    std::fclose(f);
    g_message = failed.empty() ? root + ".txt" : root + ".txt (not read: " + failed + ")";
    message = g_message.c_str();
    return failed.empty();
}

// ---- tools.clouds (render milestone 12, task 2) ----

namespace {

// Jendersie and d'Eon's Draine weight for a droplet diameter (RaCloud.fxh, Ra_CloudDroplet.x).
double drainWeight(double d)
{
    d = std::min(std::max(d, 5.0), 50.0);
    return std::exp(-0.599085 / (d - 0.641583) - 0.665888);
}

// Field slice s's density at texel (x, z) of the packed atlas (4 x 4 tiles, 4 slices each).
float fieldAt(const Table& f, unsigned int s, unsigned int x, unsigned int z)
{
    const unsigned int t = s / 4;
    return f.at((t % 4) * 512 + x, (t / 4) * 512 + z)[s % 4];
}

bool writeVolumeMontage(IDirect3DVolumeTexture9* v, const std::string& path)
{
    D3DVOLUME_DESC d;
    D3DLOCKED_BOX box;
    if (!v || v->GetLevelDesc(0, &d) < 0 || v->LockBox(0, &box, 0, D3DLOCK_READONLY) < 0)
        return false;
    const unsigned int across = 8, rows = (d.Depth + across - 1) / across;
    Table t;
    t.w = d.Width * across;
    t.h = d.Height * rows;
    t.rgba.assign(static_cast<size_t>(t.w) * t.h * 4, 0.0f);
    for (unsigned int z = 0; z < d.Depth; ++z)
        for (unsigned int y = 0; y < d.Height; ++y)
            for (unsigned int x = 0; x < d.Width; ++x)
            {
                const float c = static_cast<const unsigned char*>(box.pBits)[z * box.SlicePitch + y * box.RowPitch + x] / 255.0f;
                float* const p = &t.rgba[((static_cast<size_t>((z / across) * d.Height + y)) * t.w + (z % across) * d.Width + x) * 4];
                p[0] = p[1] = p[2] = c;
                p[3] = 1.0f;
            }
    v->UnlockBox(0);
    float scale;
    return writePng(t, path, false, scale);
}

} // namespace

bool toolsWriteClouds(const char* base, const char*& message)
{
    message = "";
    const CloudsInfo c = rendererCloudsInfo();
    if (!base || !device())
        g_message = "no renderer";
    else if (!c.live)
        g_message = "no cloud volumes yet (the atmosphere's fit, the noise or Shaders/RaClouds missing)";
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
    std::string failed;
    std::fprintf(f, "# tools.clouds (render milestone 12): the sky clouds' baked volumes, live set\n");
    std::fprintf(f, "[clouds] preset %s, coverage %.2f, life time %.0f s; slab %.0f-%.0f m above the "
                    "reference height; LWC %.2f g/m^3 at density 1, droplets %.0f um\n",
                 c.preset, c.coverage, c.seconds, c.baseM, c.topM, c.lwc, c.dropletUm);
    std::fprintf(f, "[clouds] low layer %.0f-%.0f m; mid layer %.0f-%.0f m, density %.2f, coverage %.2f\n",
                 c.lowM[0], c.lowM[1], c.midM[0], c.midM[1], c.midDensity, c.midCoverage);
    std::fprintf(f, "[clouds] noise %.0f ms (CPU, once), weather %.1f ms; a build takes %u frames; %u builds\n",
                 c.noiseMs, c.weatherMs, c.buildFrames, c.builds);

    // The weather map.
    Table weather;
    std::string why;
    if (readTable(c.weather, weather, why))
    {
        writeStats(f, "weather", weather);
        float scale;
        const std::string png = root + "-weather.png";
        if (writePng(weather, png, false, scale))
            std::fprintf(f, "[weather] png %s (r low, g mid coverage, b type; x %.4g)\n", png.c_str(), scale);
    }
    else
        failed += "weather ";

    // The noise.
    if (writeVolumeMontage(c.shape, root + "-shape.png"))
        std::fprintf(f, "[shape] png %s-shape.png (128 x 128 slices, 8 across)\n", root.c_str());
    else
        failed += "shape ";
    if (writeVolumeMontage(c.detail, root + "-detail.png"))
        std::fprintf(f, "[detail] png %s-detail.png (32 x 32 slices, 8 across)\n", root.c_str());
    else
        failed += "detail ";

    // The field: per slice, the share of texels with density > 0.02, the mean and the maximum;
    // from above, each column's optical depth (as RaCloud.fxh's extinction) and the share of the
    // sky with tau > 1 and > 10. A column-maximum image and every 4th slice at 1/4 size.
    Table field;
    if (readTable(c.field, field, why) && field.w == 2048 && field.h == 2048)
    {
        const double dz = (c.topM - c.baseM) / 64.0;
        const double sigmaPerDensity = 3.0 * c.lwc / c.dropletUm * drainWeight(c.dropletUm);
        std::vector<double> tau(512 * 512, 0.0);
        Table column;
        column.w = column.h = 512;
        column.rgba.assign(512 * 512 * 4, 0.0f);
        std::fprintf(f, "[field] per slice (height above the reference, m): share > 0.02 | mean | max\n");
        for (unsigned int s = 0; s < 64; ++s)
        {
            unsigned int n = 0;
            double sum = 0.0;
            float peak = 0.0f;
            for (unsigned int z = 0; z < 512; ++z)
                for (unsigned int x = 0; x < 512; ++x)
                {
                    const float d = fieldAt(field, s, x, z);
                    n += d > 0.02f;
                    sum += d;
                    peak = std::max(peak, d);
                    tau[z * 512 + x] += d * sigmaPerDensity * dz;
                    float* const p = &column.rgba[(z * 512 + x) * 4];
                    p[0] = p[1] = p[2] = std::max(p[0], d);
                    p[3] = 1.0f;
                }
            std::fprintf(f, "[field]  %2u %6.0f: %.4f | %.4f | %.3f\n", s, c.baseM + (s + 0.5) * dz,
                         n / 262144.0, sum / 262144.0, peak);
        }
        unsigned int over1 = 0, over10 = 0;
        double tauSum = 0.0, tauMax = 0.0;
        for (size_t i = 0; i < tau.size(); ++i)
        {
            over1 += tau[i] > 1.0;
            over10 += tau[i] > 10.0;
            tauSum += tau[i];
            tauMax = std::max(tauMax, tau[i]);
        }
        std::fprintf(f, "[field] from above: sky share with column tau > 1: %.3f, > 10: %.3f; mean tau %.2f, "
                        "max %.1f (sigma %.4f /m per unit density)\n",
                     over1 / 262144.0, over10 / 262144.0, tauSum / 262144.0, tauMax, sigmaPerDensity);
        float scale;
        if (writePng(column, root + "-field-columns.png", false, scale))
            std::fprintf(f, "[field] png %s-field-columns.png (each column's maximum, x %.4g)\n", root.c_str(), scale);
        Table slices;
        slices.w = 128 * 4;
        slices.h = 128 * 4;
        slices.rgba.assign(static_cast<size_t>(slices.w) * slices.h * 4, 0.0f);
        for (unsigned int i = 0; i < 16; ++i)
            for (unsigned int z = 0; z < 128; ++z)
                for (unsigned int x = 0; x < 128; ++x)
                {
                    const float d = fieldAt(field, i * 4 + 1, x * 4, z * 4);
                    float* const p = &slices.rgba[(((i / 4) * 128 + z) * slices.w + (i % 4) * 128 + x) * 4];
                    p[0] = p[1] = p[2] = d;
                    p[3] = 1.0f;
                }
        if (writePng(slices, root + "-field-slices.png", false, scale))
            std::fprintf(f, "[field] png %s-field-slices.png (slices 1, 5 .. 61 at 1/4, 4 across, x %.4g)\n",
                         root.c_str(), scale);

        // The coarse copy must bound the field from above.
        Table coarse;
        if (readTable(c.coarse, coarse, why) && coarse.w == 128 && coarse.h == 64)
        {
            unsigned int under = 0;
            for (unsigned int s = 0; s < 64; ++s)
                for (unsigned int z = 0; z < 512; ++z)
                    for (unsigned int x = 0; x < 512; ++x)
                    {
                        const unsigned int g = s / 8;
                        const float bound = coarse.at((g / 4) * 64 + x / 8, z / 8)[g % 4];
                        under += fieldAt(field, s, x, z) > bound + 1.0f / 255.0f;
                    }
            std::fprintf(f, "[coarse] field texels above their block's bound: %u (0 expected)\n", under);
        }
        else
            failed += "coarse ";
    }
    else
        failed += "field ";

    // The light: per slice, the mean of each channel, and a montage of the sun channel (r) and the
    // sky visibility (g), every 4th slice at 1/2 size.
    Table light;
    if (readTable(c.light, light, why) && light.w == 2048 && light.h == 2048)
    {
        std::fprintf(f, "[light] per slice: mean sun (last octave) | sky visibility | ground visibility; min sun\n");
        Table sun;
        sun.w = 128 * 8;
        sun.h = 128 * 4;
        sun.rgba.assign(static_cast<size_t>(sun.w) * sun.h * 4, 0.0f);
        for (unsigned int s = 0; s < 64; ++s)
        {
            const unsigned int ox = (s % 8) * 256, oy = (s / 8) * 256;
            double m[3] = {};
            float lo = 1e9f;
            for (unsigned int z = 0; z < 256; ++z)
                for (unsigned int x = 0; x < 256; ++x)
                {
                    const float* const p = light.at(ox + x, oy + z);
                    for (int k = 0; k < 3; ++k)
                        m[k] += p[k];
                    lo = std::min(lo, p[0]);
                }
            std::fprintf(f, "[light]  %2u: %.4f | %.4f | %.4f; %.4f\n", s, m[0] / 65536.0, m[1] / 65536.0,
                         m[2] / 65536.0, lo);
            if (s % 4 == 1)
            {
                const unsigned int i = s / 4;   // 0..15
                for (unsigned int z = 0; z < 128; ++z)
                    for (unsigned int x = 0; x < 128; ++x)
                    {
                        const float* const p = light.at(ox + x * 2, oy + z * 2);
                        float* const q = &sun.rgba[(((i / 8) * 256 + z) * sun.w + (i % 8) * 128 + x) * 4];
                        q[0] = q[1] = q[2] = p[0];
                        float* const r = &sun.rgba[(((i / 8) * 256 + 128 + z) * sun.w + (i % 8) * 128 + x) * 4];
                        r[0] = r[1] = r[2] = p[1];
                        q[3] = r[3] = 1.0f;
                    }
            }
        }
        float scale;
        if (writePng(sun, root + "-light.png", false, scale))
            std::fprintf(f, "[light] png %s-light.png (slices 1, 5 .. 61 at 1/2: rows of 8, sun then sky "
                            "visibility, x %.4g)\n", root.c_str(), scale);
    }
    else
        failed += "light ";

    // The panorama (task 3): decoded (rgb / (1 - luminance)), the light it adds and its
    // transmittance; per band of elevation, the mean transmittance and the cloud share.
    Table pano;
    if (readTable(c.panorama, pano, why) && pano.w == 2 * pano.h)
    {
        std::fprintf(f, "[panorama] %u drawn; rows by elevation (deg): mean transmittance | share < 0.5 | "
                        "mean added luminance\n", c.panoramas);
        Table alpha = pano;
        for (size_t i = 0; i < pano.rgba.size(); i += 4)
        {
            float* const p = &pano.rgba[i];
            const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            for (int k = 0; k < 3; ++k)
                p[k] /= std::max(1.0f - l, 1e-4f);
        }
        const float edges[] = { -90.0f, -10.0f, 0.0f, 2.0f, 5.0f, 10.0f, 20.0f, 40.0f, 90.0f };
        for (int b = 0; b + 1 < 9; ++b)
        {
            double t = 0.0, lum = 0.0;
            unsigned int n = 0, covered = 0;
            for (unsigned int y = 0; y < pano.h; ++y)
            {
                // Row 0 the nadir, the horizon halfway, the zenith last (RaCloudPanorama.fxh).
                const float v = 2.0f * (y + 0.5f) / pano.h - 1.0f, e = (v < 0.0f ? -v : v) * v * 90.0f;
                if (e < edges[b] || e >= edges[b + 1])
                    continue;
                for (unsigned int x = 0; x < pano.w; ++x)
                {
                    const float* const p = pano.at(x, y);
                    t += p[3];
                    covered += p[3] < 0.5f;
                    lum += 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                    ++n;
                }
            }
            if (n)
                std::fprintf(f, "[panorama]  %3.0f..%3.0f: %.3f | %.3f | %.4f\n", edges[b], edges[b + 1], t / n,
                             static_cast<double>(covered) / n, lum / n);
        }
        float scale;
        if (writePng(pano, root + "-panorama.png", false, scale))
            std::fprintf(f, "[panorama] png %s-panorama.png (row 0 the nadir, the horizon halfway, the zenith at the bottom; "
                            "x %.4g)\n", root.c_str(), scale);
        if (writePng(alpha, root + "-panorama-t.png", true, scale))
            std::fprintf(f, "[panorama] png %s-panorama-t.png (transmittance)\n", root.c_str());
    }
    else
        failed += "panorama ";

    // The near clouds (task 4): the last resolve, half the view's size, as the camera saw it.
    Table nearT;
    if (c.nearResolved && readTable(c.nearResolved, nearT, why))
    {
        double t = 0.0, lum = 0.0;
        unsigned int covered = 0;
        const size_t n = nearT.rgba.size() / 4;
        Table alpha = nearT;
        for (size_t i = 0; i < nearT.rgba.size(); i += 4)
        {
            const float* const q = &nearT.rgba[i];
            t += q[3];
            covered += q[3] < 0.5f;
            lum += 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
        }
        std::fprintf(f, "[near] %ux%u out to %.0f m: mean transmittance %.3f, share < 0.5 %.3f, mean added "
                        "luminance %.4f\n", nearT.w, nearT.h, c.nearDistance, n ? t / n : 1.0,
                     n ? static_cast<double>(covered) / n : 0.0, n ? lum / n : 0.0);
        float scale;
        if (writePng(nearT, root + "-near.png", false, scale))
            std::fprintf(f, "[near] png %s-near.png (the light added; x %.4g)\n", root.c_str(), scale);
        if (writePng(alpha, root + "-near-t.png", true, scale))
            std::fprintf(f, "[near] png %s-near-t.png (transmittance)\n", root.c_str());
    }
    else
        std::fprintf(f, "[near] none (renderer.cloudNear %.0f, or not drawn last frame)\n", c.nearDistance);

    // The cloud shadow map (task 8) and the clouds' light from the ground (task 7).
    const SkyCubeCloudLight cloudSky = rendererSkyCubeCloudLight();
    std::fprintf(f, "[skylight] ground panoramas read %u; sun transmittance %.3f, haze light %.4f %.4f %.4f\n",
                 c.skyLights, cloudSky.sunTransmittance, cloudSky.hazeLight[0], cloudSky.hazeLight[1], cloudSky.hazeLight[2]);
    Table shadowT;
    if (c.shadow && readTable(c.shadow, shadowT, why))
    {
        // Transmittance at the reference height (every receiver under the deck takes the whole column).
        double t = 0.0, enter = 0.0, leave = 0.0;
        unsigned int shaded = 0, cloudy = 0;
        const size_t n = shadowT.rgba.size() / 4;
        for (size_t i = 0; i < shadowT.rgba.size(); i += 4)
        {
            float* const q = &shadowT.rgba[i];
            const float tr = std::exp(-q[0]);
            t += tr;
            shaded += tr < 0.5f;
            if (q[0] > 0.0f)
            {
                ++cloudy;
                enter += q[1];
                leave += q[2];
            }
            q[3] = tr;
        }
        std::fprintf(f, "[shadow] %ux%u over %.0f m from (%.0f, %.0f), maps %u: mean transmittance %.3f, share < 0.5 "
                        "%.3f, mean enter %.0f m, leave %.0f m (world y)\n", shadowT.w, shadowT.h, c.shadowSize,
                     c.shadowCorner[0], c.shadowCorner[1], c.shadowMaps, n ? t / n : 1.0,
                     n ? static_cast<double>(shaded) / n : 0.0, cloudy ? enter / cloudy : 0.0,
                     cloudy ? leave / cloudy : 0.0);
        float scale;
        if (writePng(shadowT, root + "-shadow.png", true, scale))
            std::fprintf(f, "[shadow] png %s-shadow.png (transmittance at the reference height; row 0 at the "
                            "corner's z)\n", root.c_str());
    }
    else
        std::fprintf(f, "[shadow] none\n");

    std::fclose(f);
    g_message = failed.empty() ? root + ".txt" : root + ".txt (not read: " + failed + ")";
    message = g_message.c_str();
    return failed.empty();
}

// tools.exposure (render milestone 13): the auto-exposure's state, histogram and meter as the last
// frame left them, and the settings.
bool toolsWriteExposure(const char* base, const char*& message)
{
    message = "";
    const HdrPostInfo e = rendererHdrPostInfo();
    if (!base || !device())
        g_message = "no renderer";
    else if (!e.frames)
        g_message = "the HDR post has not run (Shaders/RaHdrPost missing, or bloom and exposure off)";
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
    std::string failed;
    std::fprintf(f, "# tools.exposure (render milestone 13): bloom and auto-exposure, the last frame's\n");
    std::fprintf(f, "[settings] exposure bias %.2f, min %.2f, max %.2f stops, speed %.2f (into light %.1f/s, "
                    "dark %.1f/s); key %.3f; bloom strength %.3f\n",
                 e.exposureBias, e.exposureMin, e.exposureMax, e.exposureSpeed,
                 e.rateLight * e.exposureSpeed, e.rateDark * e.exposureSpeed, e.key, e.bloomStrength);
    {
        const GradeSettings& g = rendererGrade();
        std::fprintf(f, "[grade] curve %d (%s), contrast %.3f, saturation %.3f, highlight shift %.3f, toe %.3f, "
                        "shoulder %.3f, white balance %.0f K tint %+.3f\n", g.curve,
                     g.curve ? "GT" : "PBR Neutral, fixed grade", g.contrast, g.saturation, g.highlightShift, g.toe,
                     g.shoulder, g.temperature, g.tint);
    }
    std::fprintf(f, "[frame] %u frames, last ran %s%s, dt %.4f s%s; scene %ux%u, %u bloom levels\n", e.frames,
                 e.ran ? "yes" : "no", e.thermal ? " (thermal view: skipped)" : "", e.lastDt,
                 e.lastSnap ? " (snapped)" : "", e.sceneW, e.sceneH, e.levels);

    Table state;
    std::string why;
    if (e.state && readTable(e.state, state, why))
    {
        const float* const v = state.at(0, 0);
        std::fprintf(f, "[state] adapted %.3f EV (luminance %.5f), metered %.3f EV (%.5f), exposure x%.4f "
                        "(%+.3f stops), written %.0f\n", v[0], std::exp2(v[0]), v[2], std::exp2(v[2]), v[1],
                     v[1] > 0.0f ? std::log2(v[1]) : -99.0, v[3]);
    }
    else
    {
        std::fprintf(f, "[state] none%s%s\n", e.state ? ": " : "", e.state ? why.c_str() : "");
        if (e.state)
            failed += "state ";
    }

    Table hist;
    if (e.histogram && readTable(e.histogram, hist, why))
    {
        double total = 0.0;
        for (unsigned int i = 0; i < 64; ++i)
            total += hist.at(i / 4, 0)[i % 4];
        std::fprintf(f, "[histogram] 64 bins over %.1f .. %.1f EV, total weight %.1f (bin: low EV, weight, share, "
                        "cumulative share)\n", e.evLow, e.evLow + e.evRange, total);
        double below = 0.0;
        for (unsigned int i = 0; i < 64; ++i)
        {
            const double w = hist.at(i / 4, 0)[i % 4];
            below += w;
            if (w > 0.0)
                std::fprintf(f, "  %2u %+7.3f %8.2f %.4f %.4f\n", i, e.evLow + i * e.evRange / 64.0, w,
                             total > 0.0 ? w / total : 0.0, total > 0.0 ? below / total : 0.0);
        }
    }
    else
    {
        std::fprintf(f, "[histogram] none\n");
        if (e.histogram)
            failed += "histogram ";
    }

    Table meter;
    if (e.meter && readTable(e.meter, meter, why))
    {
        float lo = 1e9f, hi = -1e9f;
        double sum = 0.0;
        for (unsigned int y = 0; y < meter.h; ++y)
            for (unsigned int x = 0; x < meter.w; ++x)
            {
                const float v = meter.at(x, y)[0];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
                sum += v;
            }
        std::fprintf(f, "[meter] %ux%u log2 luminance: min %.3f, max %.3f, mean %.3f EV; rows (every 4th, every 8th "
                        "texel):\n", meter.w, meter.h, lo, hi, sum / (meter.w * meter.h));
        for (unsigned int y = 0; y < meter.h; y += 4)
        {
            std::fprintf(f, "  %2u", y);
            for (unsigned int x = 0; x < meter.w; x += 8)
                std::fprintf(f, " %+6.2f", meter.at(x, y)[0]);
            std::fprintf(f, "\n");
        }
    }
    else
    {
        std::fprintf(f, "[meter] none\n");
        if (e.meter)
            failed += "meter ";
    }

    Table bloom;
    if (e.bloomTop && readTable(e.bloomTop, bloom, why))
    {
        writeStats(f, "bloom", bloom);
        float scale;
        if (writePng(bloom, root + "-bloom.png", false, scale))
            std::fprintf(f, "[bloom] png %s-bloom.png (level 0 after the up chain, the sum of %u levels; rgb x%.4g)\n",
                         root.c_str(), e.levels, scale);
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

// [RENDER] Sky cube, sky irradiance and the split-sum BRDF table (render milestone 11, task 7;
// docs/lanes/render.md#11-atmosphere-sky-ibl-water-reflections). Not part of retail BF2.
//
// Built on the CPU, like the atmosphere's tables, whenever the sky view is (Atmosphere.cpp: at the
// fit), into managed textures that survive device resets:
//   - the sky cube, kCubeSize per face, A16B16G16R16F: above the horizon the sky view's radiance
//     (the model's clear sky, no sun disc: the sun's highlight is the direct light's), below it the
//     ground, the colormaps' mean colour lit by the sun and this sky, under the haze toward the
//     horizon. Prefiltered for GGX: mip m holds roughness m / (kCubeMips - 1) (perceptual, alpha its
//     square), by importance sampling with the source's mip chosen per sample (filtered importance
//     sampling, Krivanek and Colbert 2008), so a few samples per texel do not alias;
//   - the sky's irradiance at a normal, as 9 spherical-harmonic coefficients per channel taken from
//     the cube's top mip and convolved with the cosine lobe (Ramamoorthi and Hanrahan 2001), divided
//     by pi (the radiance of a white diffuse surface: the shaders' irradiance unit), unscaled: the
//     model's sky under the surfaces' sun, as drawn;
//   - the split-sum BRDF table (Karis 2013), kBrdfSize square: x the cosine between the normal and
//     the view, y the perceptual roughness; r the scale and g the bias on F0 (Smith GGX, k = a / 2).
//     It does not depend on the level and is built once.
// Each frame the cube and the table are bound to the reserved samplers kSkyCubeSampler and
// kSkyBrdfSampler, and the irradiance to the engine's pixel registers kSkyShRegister.. (contract 10).
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

const double kPi = 3.14159265358979;
const unsigned int kCubeSize = 128;
const unsigned int kCubeMips = 6;          // 128 .. 4; roughness m / 5
const unsigned int kPrefilterSamples = 64;
const unsigned int kBrdfSize = 64;
const unsigned int kBrdfSamples = 256;
// The height (m) a reflection sees the ground from: a vehicle's roof, a window. Sets how close to
// the horizon the ground fades into the haze.
const double kGroundViewHeight = 10.0;

struct F3
{
    float x, y, z;
};
inline F3 f3(float x, float y, float z) { F3 v = { x, y, z }; return v; }
inline F3 operator+(const F3& a, const F3& b) { return f3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline F3 operator*(const F3& a, float s) { return f3(a.x * s, a.y * s, a.z * s); }
inline float dot(const F3& a, const F3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline F3 normalize(const F3& a)
{
    const float l = std::sqrt(dot(a, a));
    return l > 0.0f ? a * (1.0f / l) : a;
}
inline float luminance(const F3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// One mip of the cube, rgb per texel, face-major.
struct CubeMip
{
    unsigned int size;
    std::vector<F3> texels;   // 6 * size * size
    const F3& at(int face, unsigned int x, unsigned int y) const { return texels[(face * size + y) * size + x]; }
    F3& at(int face, unsigned int x, unsigned int y) { return texels[(face * size + y) * size + x]; }
};

// D3D's cube layout: the direction through texel coordinates (u, v) in [-1, 1] on each face.
F3 faceDirection(int face, float u, float v)
{
    switch (face)
    {
    case 0: return f3(1.0f, -v, -u);    // +X
    case 1: return f3(-1.0f, -v, u);    // -X
    case 2: return f3(u, 1.0f, v);      // +Y
    case 3: return f3(u, -1.0f, -v);    // -Y
    case 4: return f3(u, -v, 1.0f);     // +Z
    default: return f3(-u, -v, -1.0f);  // -Z
    }
}

F3 texelDirection(int face, unsigned int x, unsigned int y, unsigned int size)
{
    const float u = 2.0f * (x + 0.5f) / size - 1.0f;
    const float v = 2.0f * (y + 0.5f) / size - 1.0f;
    return normalize(faceDirection(face, u, v));
}

// The face and (s, t) in [0, 1] a direction lands on.
void directionToFace(const F3& d, int& face, float& s, float& t)
{
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    float u, v, m;
    if (ax >= ay && ax >= az)
    {
        m = ax;
        face = d.x > 0.0f ? 0 : 1;
        u = d.x > 0.0f ? -d.z : d.z;
        v = -d.y;
    }
    else if (ay >= az)
    {
        m = ay;
        face = d.y > 0.0f ? 2 : 3;
        u = d.x;
        v = d.y > 0.0f ? d.z : -d.z;
    }
    else
    {
        m = az;
        face = d.z > 0.0f ? 4 : 5;
        u = d.z > 0.0f ? d.x : -d.x;
        v = -d.y;
    }
    s = 0.5f * (u / m + 1.0f);
    t = 0.5f * (v / m + 1.0f);
}

// Bilinear within the face the direction lands on, clamped at its edges.
F3 sampleMip(const CubeMip& mip, const F3& d)
{
    int face;
    float s, t;
    directionToFace(d, face, s, t);
    const float fx = std::min(std::max(s * mip.size - 0.5f, 0.0f), mip.size - 1.0f);
    const float fy = std::min(std::max(t * mip.size - 0.5f, 0.0f), mip.size - 1.0f);
    const unsigned int x0 = static_cast<unsigned int>(fx), y0 = static_cast<unsigned int>(fy);
    const unsigned int x1 = std::min(x0 + 1, mip.size - 1), y1 = std::min(y0 + 1, mip.size - 1);
    const float wx = fx - x0, wy = fy - y0;
    return (mip.at(face, x0, y0) * (1.0f - wx) + mip.at(face, x1, y0) * wx) * (1.0f - wy)
         + (mip.at(face, x0, y1) * (1.0f - wx) + mip.at(face, x1, y1) * wx) * wy;
}

// Trilinear across the radiance chain.
F3 sampleChain(const std::vector<CubeMip>& chain, const F3& d, float level)
{
    const float top = static_cast<float>(chain.size() - 1);
    level = std::min(std::max(level, 0.0f), top);
    const unsigned int l0 = static_cast<unsigned int>(level);
    const unsigned int l1 = std::min(l0 + 1, static_cast<unsigned int>(top));
    const float w = level - l0;
    if (w <= 0.0f || l0 == l1)
        return sampleMip(chain[l0], d);
    return sampleMip(chain[l0], d) * (1.0f - w) + sampleMip(chain[l1], d) * w;
}

// The solid angle of texel (x, y) on a face of the given size (Driscoll's area element).
float texelSolidAngle(unsigned int x, unsigned int y, unsigned int size)
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

float radicalInverse(unsigned int bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

// A GGX half vector around +Z for sample (u1, u2), alpha = roughness^2.
F3 ggxHalf(float u1, float u2, float alpha)
{
    const float phi = static_cast<float>(2.0 * kPi) * u1;
    const float cosTheta = std::sqrt((1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2));
    const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
    return f3(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
}

float ggxD(float nh, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (static_cast<float>(kPi) * d * d);
}

void tangentFrame(const F3& n, F3& t, F3& b)
{
    const F3 up = std::fabs(n.y) < 0.999f ? f3(0, 1, 0) : f3(1, 0, 0);
    t = normalize(f3(up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z, up.x * n.y - up.y * n.x));
    b = f3(n.y * t.z - n.z * t.y, n.z * t.x - n.x * t.z, n.x * t.y - n.y * t.x);
}

struct State
{
    IDirect3DCubeTexture9* cube;
    IDirect3DTexture9* brdf;
    bool brdfBuilt;
    bool live;                // the cube and the irradiance are built
    float sh[7][4];           // RaSkySH, packed (contract 10)
    float irradianceUp[3];    // E(up) / pi
    float buildMs;
};

State g = {};

IDirect3DDevice9* device()
{
    return g_rendererBase ? static_cast<RendDX9Base*>(g_rendererBase)->device : 0;
}

unsigned short floatToHalf(float f)
{
    unsigned int bits;
    std::memcpy(&bits, &f, sizeof(bits));
    const unsigned int sign = (bits >> 16) & 0x8000u;
    const int exponent = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
    const unsigned int mantissa = bits & 0x7FFFFFu;
    if (exponent <= 0)
        return static_cast<unsigned short>(sign);
    if (exponent >= 31)
        return static_cast<unsigned short>(sign | 0x7BFFu);
    unsigned int h = sign | (static_cast<unsigned int>(exponent) << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u)
        ++h;
    return static_cast<unsigned short>(h);
}

void writeHalfRow(void* dst, const F3* src, unsigned int count, float alpha)
{
    unsigned short* const row = static_cast<unsigned short*>(dst);
    for (unsigned int x = 0; x < count; ++x)
    {
        row[x * 4 + 0] = floatToHalf(src[x].x);
        row[x * 4 + 1] = floatToHalf(src[x].y);
        row[x * 4 + 2] = floatToHalf(src[x].z);
        row[x * 4 + 3] = floatToHalf(alpha);
    }
}

// The radiance toward d: the sky view above the horizon, the hazed ground below it.
struct Radiance
{
    const SkyCubeSource* src;
    double zenithHorizon, beta;
    F3 sunHorizontal;
    F3 ground;           // the lit ground's radiance

    F3 skyView(double u, double v) const
    {
        const double fx = std::min(std::max(u * src->skyViewW - 0.5, 0.0), src->skyViewW - 1.0);
        const double fy = std::min(std::max(v * src->skyViewH - 0.5, 0.0), src->skyViewH - 1.0);
        const unsigned int x0 = static_cast<unsigned int>(fx), y0 = static_cast<unsigned int>(fy);
        const unsigned int x1 = std::min(x0 + 1, src->skyViewW - 1), y1 = std::min(y0 + 1, src->skyViewH - 1);
        const float wx = static_cast<float>(fx - x0), wy = static_cast<float>(fy - y0);
        const float* const t = src->skyView;
        const unsigned int w = src->skyViewW;
        F3 c[4];
        const unsigned int idx[4] = { y0 * w + x0, y0 * w + x1, y1 * w + x0, y1 * w + x1 };
        for (int i = 0; i < 4; ++i)
            c[i] = f3(t[idx[i] * 3 + 0], t[idx[i] * 3 + 1], t[idx[i] * 3 + 2]);
        return (c[0] * (1.0f - wx) + c[1] * wx) * (1.0f - wy) + (c[2] * (1.0f - wx) + c[3] * wx) * wy;
    }

    // As Ra_SkyViewRadiance (RaAtmosphere.fxh): at and below the horizon, the horizon's.
    F3 sky(const F3& d) const
    {
        const double zenith = std::acos(std::min(std::max(static_cast<double>(d.y), -1.0), 1.0));
        double v;
        if (zenith < zenithHorizon)
            v = 0.5 * (1.0 - std::sqrt(std::max(0.0, 1.0 - zenith / zenithHorizon)));
        else
            v = 0.5 * (1.0 + std::sqrt(std::max(0.0, (zenith - zenithHorizon) / beta)));
        v = std::min(v, 0.5 - 0.5 / src->skyViewH);
        const double hl = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.z) * d.z);
        const double sl = std::sqrt(static_cast<double>(sunHorizontal.x) * sunHorizontal.x
                                    + static_cast<double>(sunHorizontal.z) * sunHorizontal.z);
        const double cosAz = hl * sl > 1e-12 ? (d.x * sunHorizontal.x + d.z * sunHorizontal.z) / (hl * sl) : 1.0;
        const double u = std::sqrt(std::min(std::max(0.5 * (1.0 - cosAz), 0.0), 1.0));
        return skyView(u, v);
    }

    F3 operator()(const F3& d) const
    {
        const F3 horizonOrSky = sky(d);
        if (d.y >= 0.0f)
            return horizonOrSky;
        // Seen from kGroundViewHeight: the ground at height / sin(depression), hazed over that path
        // by the haze's density at the reference height; full haze is the horizon's radiance.
        const double depth = src->hazeDensity * kGroundViewHeight / std::max(static_cast<double>(-d.y), 1e-4);
        const float clear = static_cast<float>(std::exp(-depth));
        return ground * clear + horizonOrSky * (1.0f - clear);
    }
};

// The sky's irradiance / pi on an up-facing surface (the cosine-weighted mean of the sky view's
// radiance over the upper hemisphere), which lights the ground below the horizon.
F3 skyIrradianceUp(const Radiance& radiance)
{
    const int kZenith = 32, kAzimuth = 64;
    F3 e = f3(0, 0, 0);
    double weight = 0.0;
    for (int i = 0; i < kZenith; ++i)
    {
        const double theta = (i + 0.5) * 0.5 * kPi / kZenith;
        const double w = std::cos(theta) * std::sin(theta);   // cos x the solid angle's sin
        for (int j = 0; j < kAzimuth; ++j)
        {
            const double phi = (j + 0.5) * 2.0 * kPi / kAzimuth;
            const F3 d = f3(static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::cos(theta)),
                            static_cast<float>(std::sin(theta) * std::sin(phi)));
            e = e + radiance.sky(d) * static_cast<float>(w);
            weight += w;
        }
    }
    // E / pi = (1 / pi) * integral(L cos) = sum(L w) / sum(w): the cosine integral's own norm is pi
    return weight > 0.0 ? e * static_cast<float>(1.0 / weight) : e;
}

// 2x2 box down to 1 texel per face.
void buildChain(std::vector<CubeMip>& chain)
{
    while (chain.back().size > 1)
    {
        const CubeMip& src = chain.back();
        CubeMip dst;
        dst.size = src.size / 2;
        dst.texels.resize(6 * dst.size * dst.size);
        for (int face = 0; face < 6; ++face)
            for (unsigned int y = 0; y < dst.size; ++y)
                for (unsigned int x = 0; x < dst.size; ++x)
                    dst.at(face, x, y) = (src.at(face, 2 * x, 2 * y) + src.at(face, 2 * x + 1, 2 * y)
                                          + src.at(face, 2 * x, 2 * y + 1) + src.at(face, 2 * x + 1, 2 * y + 1)) * 0.25f;
        chain.push_back(dst);
    }
}

// GGX prefilter of mip `level` (roughness level / (kCubeMips - 1)), assuming the view along the
// normal (n = v = r).
void prefilter(const std::vector<CubeMip>& chain, unsigned int level, CubeMip& out)
{
    out.size = kCubeSize >> level;
    out.texels.resize(6 * out.size * out.size);
    if (level == 0)
    {
        out.texels = chain[0].texels;
        return;
    }
    const float roughness = static_cast<float>(level) / (kCubeMips - 1);
    const float alpha = roughness * roughness;
    const float texelSolid = static_cast<float>(4.0 * kPi) / (6.0f * kCubeSize * kCubeSize);
    // The samples in the tangent frame, shared by every texel.
    struct Sample
    {
        F3 l;          // tangent space, z the normal
        float weight;  // n.l
        float level;   // the source mip
    };
    std::vector<Sample> samples;
    for (unsigned int i = 0; i < kPrefilterSamples; ++i)
    {
        const F3 h = ggxHalf((i + 0.5f) / kPrefilterSamples, radicalInverse(i), alpha);
        const F3 l = f3(2.0f * h.z * h.x, 2.0f * h.z * h.y, 2.0f * h.z * h.z - 1.0f);
        if (l.z <= 0.0f)
            continue;
        const float pdf = ggxD(h.z, alpha) * 0.25f;   // D * n.h / (4 v.h), n = v
        const float sampleSolid = 1.0f / (kPrefilterSamples * pdf + 1e-6f);
        Sample s;
        s.l = l;
        s.weight = l.z;
        s.level = std::max(0.5f * std::log(sampleSolid / texelSolid) / std::log(2.0f) + 1.0f, 0.0f);
        samples.push_back(s);
    }
    for (int face = 0; face < 6; ++face)
        for (unsigned int y = 0; y < out.size; ++y)
            for (unsigned int x = 0; x < out.size; ++x)
            {
                const F3 n = texelDirection(face, x, y, out.size);
                F3 t, b;
                tangentFrame(n, t, b);
                F3 sum = f3(0, 0, 0);
                float weight = 0.0f;
                for (size_t i = 0; i < samples.size(); ++i)
                {
                    const Sample& s = samples[i];
                    const F3 l = t * s.l.x + b * s.l.y + n * s.l.z;
                    sum = sum + sampleChain(chain, l, s.level) * s.weight;
                    weight += s.weight;
                }
                out.at(face, x, y) = weight > 0.0f ? sum * (1.0f / weight) : f3(0, 0, 0);
            }
}

// The 9 SH coefficients of the radiance (rgb), from the top mip: the sky (above the horizon) and
// the ground (below it) apart, since only the sky is scaled.
void projectSh(const CubeMip& mip, F3 sky[9], F3 ground[9])
{
    for (int i = 0; i < 9; ++i)
        sky[i] = ground[i] = f3(0, 0, 0);
    for (int face = 0; face < 6; ++face)
        for (unsigned int y = 0; y < mip.size; ++y)
            for (unsigned int x = 0; x < mip.size; ++x)
            {
                const F3 d = texelDirection(face, x, y, mip.size);
                const F3 l = mip.at(face, x, y) * texelSolidAngle(x, y, mip.size);
                const float basis[9] = {
                    0.282095f,
                    0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
                    1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.0f * d.z * d.z - 1.0f),
                    1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y) };
                F3* const c = d.y >= 0.0f ? sky : ground;
                for (int i = 0; i < 9; ++i)
                    c[i] = c[i] + l * basis[i];
            }
}

// Irradiance / pi from radiance coefficients: the cosine lobe's band factors (pi, 2 pi / 3, pi / 4)
// over pi.
void convolveSh(F3 c[9])
{
    static const float kBand[9] = { 1.0f, 2.0f / 3, 2.0f / 3, 2.0f / 3, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f };
    for (int i = 0; i < 9; ++i)
        c[i] = c[i] * kBand[i];
}

F3 evalSh(const F3 c[9], const F3& d)
{
    const float basis[9] = {
        0.282095f,
        0.488603f * d.y, 0.488603f * d.z, 0.488603f * d.x,
        1.092548f * d.x * d.y, 1.092548f * d.y * d.z, 0.315392f * (3.0f * d.z * d.z - 1.0f),
        1.092548f * d.x * d.z, 0.546274f * (d.x * d.x - d.y * d.y) };
    F3 r = f3(0, 0, 0);
    for (int i = 0; i < 9; ++i)
        r = r + c[i] * basis[i];
    return r;
}

// RaSkySH (contract 10), 7 float4s; per channel ch:
//   [ch]     (x, y, z, 1) coefficients: c3 * k1, c1 * k1, c2 * k1, c0 * k0 - c6 * k20
//   [3 + ch] (xy, yz, zz, zx) coefficients: c4 * k2, c5 * k2, c6 * 3 * k20, c7 * k2
//   [6]      rgb: c8 * k22 for x^2 - y^2; w 1 while live
// so irradiance / pi at n = dot([ch], (n, 1)) + dot([3 + ch], n.xyzz * n.yzzx) + [6][ch] * (n.x^2 - n.y^2).
void packSh(const F3 c[9], float out[7][4])
{
    const float k0 = 0.282095f, k1 = 0.488603f, k2 = 1.092548f, k20 = 0.315392f, k22 = 0.546274f;
    for (int ch = 0; ch < 3; ++ch)
    {
        const float* const p[9] = { &c[0].x + ch, &c[1].x + ch, &c[2].x + ch, &c[3].x + ch, &c[4].x + ch,
                                    &c[5].x + ch, &c[6].x + ch, &c[7].x + ch, &c[8].x + ch };
        out[ch][0] = *p[3] * k1;
        out[ch][1] = *p[1] * k1;
        out[ch][2] = *p[2] * k1;
        out[ch][3] = *p[0] * k0 - *p[6] * k20;
        out[3 + ch][0] = *p[4] * k2;
        out[3 + ch][1] = *p[5] * k2;
        out[3 + ch][2] = *p[6] * 3.0f * k20;
        out[3 + ch][3] = *p[7] * k2;
        out[6][ch] = *p[8] * k22;
    }
    out[6][3] = 1.0f;
}

// The split-sum table: x n.v, y perceptual roughness; (scale, bias) on F0.
void buildBrdf(std::vector<F3>& out)
{
    out.resize(kBrdfSize * kBrdfSize);
    for (unsigned int y = 0; y < kBrdfSize; ++y)
    {
        const float roughness = (y + 0.5f) / kBrdfSize;
        const float alpha = roughness * roughness;
        const float k = alpha * 0.5f;
        for (unsigned int x = 0; x < kBrdfSize; ++x)
        {
            const float nv = (x + 0.5f) / kBrdfSize;
            const F3 v = f3(std::sqrt(1.0f - nv * nv), 0.0f, nv);
            float a = 0.0f, b = 0.0f;
            for (unsigned int i = 0; i < kBrdfSamples; ++i)
            {
                const F3 h = ggxHalf((i + 0.5f) / kBrdfSamples, radicalInverse(i), alpha);
                const float vh = dot(v, h);
                const F3 l = h * (2.0f * vh) + v * -1.0f;
                const float nl = std::max(l.z, 0.0f), nh = std::max(h.z, 0.0f);
                if (nl <= 0.0f)
                    continue;
                const float gv = nv / (nv * (1.0f - k) + k), gl = nl / (nl * (1.0f - k) + k);
                const float gVis = gv * gl * std::max(vh, 0.0f) / (nh * nv);
                const float fc = std::pow(1.0f - std::max(vh, 0.0f), 5.0f);
                a += (1.0f - fc) * gVis;
                b += fc * gVis;
            }
            out[y * kBrdfSize + x] = f3(a / kBrdfSamples, b / kBrdfSamples, 0.0f);
        }
    }
}

bool uploadBrdf()
{
    std::vector<F3> table;
    buildBrdf(table);
    if (!g.brdf && device()->CreateTexture(kBrdfSize, kBrdfSize, 1, 0, D3DFMT_A16B16G16R16F,
                                           D3DPOOL_MANAGED, &g.brdf, 0) < 0)
    {
        g.brdf = 0;
        std::fprintf(stderr, "[SKYCUBE] BRDF table not created\n");
        return false;
    }
    D3DLOCKED_RECT lr;
    if (g.brdf->LockRect(0, &lr, 0, 0) < 0)
        return false;
    for (unsigned int y = 0; y < kBrdfSize; ++y)
        writeHalfRow(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, &table[y * kBrdfSize], kBrdfSize, 1.0f);
    g.brdf->UnlockRect(0);
    return true;
}

bool uploadCube(const std::vector<CubeMip>& mips)
{
    if (!g.cube && device()->CreateCubeTexture(kCubeSize, kCubeMips, 0, D3DFMT_A16B16G16R16F,
                                               D3DPOOL_MANAGED, &g.cube, 0) < 0)
    {
        g.cube = 0;
        std::fprintf(stderr, "[SKYCUBE] cube not created\n");
        return false;
    }
    for (unsigned int level = 0; level < kCubeMips; ++level)
        for (int face = 0; face < 6; ++face)
        {
            D3DLOCKED_RECT lr;
            if (g.cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &lr, 0, 0) < 0)
                return false;
            const CubeMip& m = mips[level];
            for (unsigned int y = 0; y < m.size; ++y)
                writeHalfRow(static_cast<unsigned char*>(lr.pBits) + y * lr.Pitch, &m.at(face, 0, y), m.size, 1.0f);
            g.cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
        }
    return true;
}

void bindSampler(unsigned int stage, IDirect3DBaseTexture9* texture, bool mips)
{
    IDirect3DDevice9* const d = device();
    d->SetTexture(stage, texture);
    if (!texture)
        return;
    d->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d->SetSamplerState(stage, D3DSAMP_MIPFILTER, mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
    d->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
    d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

} // anonymous namespace

void rendererSkyCubeBuild(const SkyCubeSource& src)
{
    if (!device() || !src.skyView || !src.skyViewW || !src.skyViewH)
        return;
    const LARGE_INTEGER zero = {};
    LARGE_INTEGER t0 = zero, t1 = zero, freq = zero;
    QueryPerformanceCounter(&t0);

    if (!g.brdfBuilt)
        g.brdfBuilt = uploadBrdf();

    Radiance radiance;
    radiance.src = &src;
    const double r = src.groundRadiusKm + src.altitudeKm;
    radiance.beta = std::acos(std::sqrt(src.altitudeKm * (2.0 * src.groundRadiusKm + src.altitudeKm)) / r);
    radiance.zenithHorizon = kPi - radiance.beta;
    radiance.sunHorizontal = f3(src.toSun[0], 0.0f, src.toSun[2]);
    const float sunCos = std::max(src.toSun[1], 0.0f);
    const F3 skyUp = skyIrradianceUp(radiance);
    radiance.ground = f3(src.groundAlbedo[0] * (src.sun[0] * sunCos + skyUp.x),
                         src.groundAlbedo[1] * (src.sun[1] * sunCos + skyUp.y),
                         src.groundAlbedo[2] * (src.sun[2] * sunCos + skyUp.z));

    // The radiance at the top mip, then its box chain for the prefilter's sources.
    std::vector<CubeMip> chain(1);
    chain[0].size = kCubeSize;
    chain[0].texels.resize(6 * kCubeSize * kCubeSize);
    for (int face = 0; face < 6; ++face)
        for (unsigned int y = 0; y < kCubeSize; ++y)
            for (unsigned int x = 0; x < kCubeSize; ++x)
                chain[0].at(face, x, y) = radiance(texelDirection(face, x, y, kCubeSize));
    buildChain(chain);

    std::vector<CubeMip> mips(kCubeMips);
    for (unsigned int level = 0; level < kCubeMips; ++level)
        prefilter(chain, level, mips[level]);

    // The irradiance, sky and ground together, as drawn.
    F3 c[9], ground[9];
    projectSh(chain[0], c, ground);
    convolveSh(c);
    convolveSh(ground);
    for (int i = 0; i < 9; ++i)
        c[i] = c[i] + ground[i];
    const F3 up = evalSh(c, f3(0, 1, 0));
    g.irradianceUp[0] = up.x;
    g.irradianceUp[1] = up.y;
    g.irradianceUp[2] = up.z;
    packSh(c, g.sh);

    g.live = uploadCube(mips);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    g.buildMs = freq.QuadPart ? static_cast<float>(1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart) : 0.0f;
    std::fprintf(stderr, "[SKYCUBE] %s in %.0f ms: irradiance up %.4f %.4f %.4f, ground %.4f %.4f %.4f\n",
                 g.live ? "built" : "FAILED", g.buildMs, up.x, up.y, up.z,
                 radiance.ground.x, radiance.ground.y, radiance.ground.z);
}

void rendererSkyCubeFrame(bool use)
{
    if (!device())
        return;
    static const float kOff[7][4] = {};
    const bool live = use && g.live;
    stateManagerSetSkySh(live ? &g.sh[0][0] : &kOff[0][0]);
    bindSampler(kSkyCubeSampler, live ? g.cube : 0, true);
    bindSampler(kSkyBrdfSampler, live && g.brdfBuilt ? g.brdf : 0, false);
}

SkyCubeInfo rendererSkyCubeInfo()
{
    SkyCubeInfo i;
    std::memset(&i, 0, sizeof(i));
    i.live = g.live;
    i.cube = g.cube;
    i.brdf = g.brdf;
    i.cubeSize = kCubeSize;
    i.cubeMips = kCubeMips;
    i.brdfSize = kBrdfSize;
    i.buildMs = g.buildMs;
    std::memcpy(i.sh, g.sh, sizeof(i.sh));
    std::memcpy(i.irradianceUp, g.irradianceUp, sizeof(i.irradianceUp));
    return i;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

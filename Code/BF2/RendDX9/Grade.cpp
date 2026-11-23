// [RENDER] The frame grade's look (render milestone 21, docs/lanes/render.md#21-tonemap-and-grade).
// Not part of retail BF2.
//
// The grade itself is FSQuadDrawer pass 0 (RGrade_Apply); this file holds its console variables and
// hands them to the effect as RaGradeLook (float4[8], layout below), next to HdrPost.cpp's RaGrade.
// White balance is a von Kries adaptation in Bradford cone space, from the white of
// renderer.gradeTemperature (Planckian locus, Kang et al. 2002) shifted off the locus by
// renderer.gradeTint, to D65; computed here once per change, a 3x3 matrix for the shader. The locus
// point is offset so that 6504 K with no tint is D65 itself (the locus passes about 0.003 Duv off it),
// so the default is the identity.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"

#include <cmath>

namespace dice {
namespace hfe {
namespace dx9rend {

namespace {

GradeSettings g_grade;

struct Mat3
{
    double m[3][3];
};

Mat3 mul(const Mat3& a, const Mat3& b)
{
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
    return r;
}

void apply(const Mat3& a, const double v[3], double out[3])
{
    for (int i = 0; i < 3; ++i)
        out[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2];
}

const Mat3 kRgbToXyz = { { { 0.4124564, 0.3575761, 0.1804375 },
                           { 0.2126729, 0.7151522, 0.0721750 },
                           { 0.0193339, 0.1191920, 0.9503041 } } };
const Mat3 kXyzToRgb = { { { 3.2404542, -1.5371385, -0.4985314 },
                           { -0.9692660, 1.8760108, 0.0415560 },
                           { 0.0556434, -0.2040259, 1.0572252 } } };
const Mat3 kBradford = { { { 0.8951, 0.2664, -0.1614 },
                           { -0.7502, 1.7135, 0.0367 },
                           { 0.0389, -0.0685, 1.0296 } } };
const Mat3 kBradfordInv = { { { 0.9869929, -0.1470543, 0.1599627 },
                              { 0.4323053, 0.5183603, 0.0492912 },
                              { -0.0085287, 0.0400428, 0.9684867 } } };
const double kD65x = 0.31271, kD65y = 0.32902;

// The Planckian locus in CIE 1960 uv (Kang et al. 2002, 1667-25000 K)
void locusUv(double t, double& u, double& v)
{
    const double t2 = t * t, t3 = t2 * t;
    const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                                 : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
    const double x2 = x * x, x3 = x2 * x;
    const double y = t <= 2222.0 ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683
                   : t <= 4000.0 ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867
                                 : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    const double d = -2.0 * x + 12.0 * y + 3.0;
    u = 4.0 * x / d;
    v = 6.0 * y / d;
}

// The illuminant the image is balanced for, as XYZ with Y = 1
void sourceWhite(double kelvin, double tint, double xyz[3])
{
    double u, v, u0, v0, ua, va, ub, vb;
    locusUv(kelvin, u, v);
    locusUv(6504.0, u0, v0);
    const double dd = -2.0 * kD65x + 12.0 * kD65y + 3.0;
    u += 4.0 * kD65x / dd - u0;
    v += 6.0 * kD65y / dd - v0;
    // Tint: along the locus' normal, + toward green (the image goes magenta), 1 = 0.02 Duv
    locusUv(kelvin - 10.0, ua, va);
    locusUv(kelvin + 10.0, ub, vb);
    double nu = vb - va, nv = -(ub - ua);
    const double len = std::sqrt(nu * nu + nv * nv);
    if (len > 0.0)
    {
        nu /= len;
        nv /= len;
        if (nv < 0.0)
        {
            nu = -nu;
            nv = -nv;
        }
        u += 0.02 * tint * nu;
        v += 0.02 * tint * nv;
    }
    const double d = 2.0 * u - 8.0 * v + 4.0;
    const double x = 3.0 * u / d, y = 2.0 * v / d;
    xyz[0] = x / y;
    xyz[1] = 1.0;
    xyz[2] = (1.0 - x - y) / y;
}

Mat3 whiteBalance(double kelvin, double tint)
{
    double src[3], dst[3], srcLms[3], dstLms[3];
    sourceWhite(kelvin, tint, src);
    dst[0] = kD65x / kD65y;
    dst[1] = 1.0;
    dst[2] = (1.0 - kD65x - kD65y) / kD65y;
    apply(kBradford, src, srcLms);
    apply(kBradford, dst, dstLms);
    Mat3 scale = { { { dstLms[0] / srcLms[0], 0.0, 0.0 },
                     { 0.0, dstLms[1] / srcLms[1], 0.0 },
                     { 0.0, 0.0, dstLms[2] / srcLms[2] } } };
    Mat3 r = mul(kXyzToRgb, mul(kBradfordInv, mul(scale, mul(kBradford, kRgbToXyz))));
    // Keep a white surface's luminance: the balance shifts colour, the exposure owns brightness
    double white[3], one[3] = { 1.0, 1.0, 1.0 };
    apply(r, one, white);
    const double y = 0.2126 * white[0] + 0.7152 * white[1] + 0.0722 * white[2];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] /= y;
    return r;
}

} // anonymous namespace

GradeSettings& rendererGrade()
{
    return g_grade;
}

void rendererGradeBind(void* effect, bool thermal)
{
    ID3DXEffect* const d3dx = static_cast<ID3DXEffect*>(effect);
    const D3DXHANDLE h = d3dx ? d3dx->GetParameterBySemantic(0, "RaGradeLook") : 0;
    if (!h)
        return;
    const GradeSettings& g = g_grade;
    // RaGradeLook (FSQuadDrawer.fx):
    //   [0] x the curve (0 PBR Neutral, retail-era constants; 1 the GT curve), y contrast,
    //       z saturation, w the highlights' shift toward white per channel (0-1)
    //   [1] x toe (the toe's power), y shoulder (the linear section's length, 0-1), zw 0
    //   [2..4] the white balance, rows of a 3x3 matrix on linear sRGB (w 0)
    //   [5] the shadows' tint (luminance 1), w their saturation
    //   [6] the mids' tint (luminance 1), w 0
    //   [7] the highlights' tint (luminance 1), w their saturation
    // Thermals keep curve 0: their encode is heat, not radiance.
    static double cachedKelvin = -1.0, cachedTint = -1.0;
    static Mat3 wb;
    if (cachedKelvin != g.temperature || cachedTint != g.tint)
    {
        wb = whiteBalance(g.temperature, g.tint);
        cachedKelvin = g.temperature;
        cachedTint = g.tint;
    }
    float v[8][4] = {
        { thermal ? 0.0f : static_cast<float>(g.curve), g.contrast, g.saturation, g.highlightShift },
        { g.toe, g.shoulder, 0.0f, 0.0f },
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            v[2 + i][j] = static_cast<float>(wb.m[i][j]);
    const float* const tints[3] = { g.shadowTint, g.midTint, g.highlightTint };
    for (int r = 0; r < 3; ++r)
    {
        const float* const t = tints[r];
        const float y = 0.2126f * t[0] + 0.7152f * t[1] + 0.0722f * t[2];
        for (int j = 0; j < 3; ++j)
            v[5 + r][j] = y > 1e-4f ? t[j] / y : 1.0f;
    }
    v[5][3] = g.shadowSaturation;
    v[7][3] = g.highlightSaturation;
    d3dx->SetVectorArray(h, reinterpret_cast<const D3DXVECTOR4*>(v), 8);
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

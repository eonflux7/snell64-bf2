// [RENDER] Water's medium: the water type (render milestone 23, task 4,
// docs/milestones/render-23-water-shore.md). Not part of retail BF2, whose water took its colour from
// the level's authored WaterColor (no longer read by the water body).
//
// The water is pure water plus three constituents, each with a published bio-optical model:
//
//   pure water      absorption a_w: Pope and Fry (1997), 400-700 nm every 10 nm; scattering
//                   b_w = 0.00288 (lambda / 500)^-4.32 (Morel 1974, seawater), half of it backward
//   chlorophyll C   (mg/m^3) phytoplankton absorption a_ph(440) = 0.0654 C^0.728 (Bricaud et al.
//                   1998) on a mean spectral shape; scattering b_p(550) = 0.30 C^0.62 (550 / lambda)
//                   (Gordon and Morel 1983), backscattered by 0.002 + 0.01 (0.5 - 0.25 log10 C)
//                   (Morel and Maritorena 2001)
//   CDOM            dissolved organic matter, its absorption at 440 nm (per m) falling as
//                   e^(-0.015 (lambda - 440)) (Bricaud et al. 1981)
//   sediment S      (g/m^3) mineral particles: absorption 0.031 S e^(-0.0123 (lambda - 443)) and
//                   scattering 0.51 S (555 / lambda)^0.5 (Babin et al. 2003), backscattered by 0.0183
//                   (Petzold's turbid harbour)
//
// From a(lambda) and b_b(lambda), per screen channel (linear sRGB's response from the CIE 1931
// observer, Wyman, Sloan and Shirley's fit, flat light): the diffuse attenuation K = a + b_b, the
// light's loss down to the bed and back up and along the view (forward scattering stays on the
// path; the bed's blur is not modelled), taken as the channel's mean transmittance over 3 m turned
// back into a coefficient (a band's mean of e^(-K d) is not e^(-mean K d)); and b_b, the channel's
// mean, which the shader scatters isotropically as 2 b_b (an isotropic medium backscatters half).
// Deep water's reflectance then comes out within about 10 % of Gordon's 0.089 b_b / (a + b_b).
//
// renderer.waterType picks a preset of the three, near Jerlov's classification by its diffuse
// attenuation (Jerlov 1976: oceanic I to III, coastal 1 to 9) and a glacial river (silt);
// renderer.waterChlorophyll, renderer.waterCdom and renderer.waterSediment change one constituent
// (waterType then reads -1, custom). A level's type is in its weather file (Weather.cpp); a level
// without one is coastal 1, clear coastal water. Published as RaWaterMedium (kWaterMediumRegister,
// four float4s: K, b_b, and per g/m^3 of sediment stirred up by the surf (task 5) what it adds to
// each, with renderer.waterSurfSediment) every frame from rendererWaterFrame.
//
// Surf turbidity (task 5): breaking waves hold fine sediment in suspension, a mineral like the
// constituent above. Its increments to K and b_b per g/m^3 are the medium's with kSurfReference
// g/m^3 more sediment, less the medium's, over kSurfReference (linear in the concentration but
// for each channel's band mean). The shader sets the concentration from the breakers' height
// (RaShore.fxh, Ra_SurfSediment): renderer.waterSurfSediment g/m^3 per metre of it.
#ifdef BF2_IMAGE_RENDDX9

#define NOMINMAX
#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "RendDX9Base.h"

#include <algorithm>
#include <cmath>

namespace dice {
namespace hfe {
namespace dx9rend {

namespace {

const int kBands = 31;   // 400-700 nm, 10 nm apart
// Pure water's absorption, per metre (Pope and Fry 1997; 700 nm Smith and Baker 1981)
const double kPureWater[kBands] = {
    0.00663, 0.00473, 0.00454, 0.00495, 0.00635, 0.00922, 0.00979, 0.0106, 0.0127, 0.0150, 0.0204,
    0.0325,  0.0409,  0.0434,  0.0474,  0.0565,  0.0619,  0.0695,  0.0896, 0.1351, 0.2224, 0.2644,
    0.2755,  0.2916,  0.3108,  0.3400,  0.4100,  0.4390,  0.4650,  0.5160, 0.6240,
};
// Phytoplankton absorption's mean shape, 1 at 440 nm (the blue Soret and the red 675 nm peaks)
const double kPhytoShape[kBands] = {
    0.69, 0.78, 0.86, 0.94, 1.00, 0.97, 0.92, 0.86, 0.78, 0.68, 0.58, 0.49, 0.40, 0.32, 0.26, 0.21,
    0.17, 0.15, 0.14, 0.14, 0.14, 0.15, 0.16, 0.18, 0.20, 0.25, 0.33, 0.48, 0.45, 0.25, 0.12,
};

struct Preset
{
    const char* name;
    float chlorophyll;   // mg/m^3
    float cdom;          // a_g(440), per m
    float sediment;      // g/m^3
};
// Each near its Jerlov type's diffuse attenuation at 450, 550 and 600 nm
const Preset kPresets[] = {
    { "oceanic I", 0.03f, 0.003f, 0.0f },   { "oceanic IA", 0.08f, 0.006f, 0.0f },
    { "oceanic IB", 0.15f, 0.01f, 0.0f },   { "oceanic II", 0.3f, 0.012f, 0.05f },
    { "oceanic III", 0.8f, 0.03f, 0.2f },   { "coastal 1", 1.0f, 0.08f, 1.0f },
    { "coastal 3", 2.0f, 0.15f, 2.5f },     { "coastal 5", 3.0f, 0.28f, 3.5f },
    { "coastal 7", 4.0f, 0.45f, 5.0f },     { "coastal 9", 5.0f, 0.7f, 8.0f },
    { "glacial river", 0.1f, 0.05f, 30.0f },
};
const int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);
const int kDefaultPreset = 5;   // coastal 1

struct State
{
    int type = kDefaultPreset;
    float chlorophyll = kPresets[kDefaultPreset].chlorophyll;
    float cdom = kPresets[kDefaultPreset].cdom;
    float sediment = kPresets[kDefaultPreset].sediment;
    float surf = 40.0f;   // g/m^3 per metre of breaker height (renderer.waterSurfSediment)
    bool dirty = true;
    float medium[16] = {};
};
const double kSurfReference = 50.0;   // g/m^3 added to find the surf sediment's increments
State g_state;

double lobe(double l, double mu, double s1, double s2)
{
    const double t = (l - mu) / (l < mu ? s1 : s2);
    return std::exp(-0.5 * t * t);
}

// Linear sRGB's response at wavelength l (nm), negative lobes clipped
void response(double l, double rgb[3])
{
    const double x = 1.056 * lobe(l, 599.8, 37.9, 31.0) + 0.362 * lobe(l, 442.0, 16.0, 26.7) -
                     0.065 * lobe(l, 501.1, 20.4, 26.2);
    const double y = 0.821 * lobe(l, 568.8, 46.9, 40.5) + 0.286 * lobe(l, 530.9, 16.3, 31.1);
    const double z = 1.217 * lobe(l, 437.0, 11.8, 36.0) + 0.681 * lobe(l, 459.0, 26.0, 13.8);
    rgb[0] = std::max(0.0, 3.2406 * x - 1.5372 * y - 0.4986 * z);
    rgb[1] = std::max(0.0, -0.9689 * x + 1.8758 * y + 0.0415 * z);
    rgb[2] = std::max(0.0, 0.0557 * x - 0.2040 * y + 1.0570 * z);
}

// K and b_b per channel (rgb), with Extra g/m^3 of sediment added
void channels(const State& s, double extra, double kOut[3], double bbOut[3])
{
    const double kDepth = 3.0;   // metres over which a channel's mean transmittance is matched
    const double c = std::max(static_cast<double>(s.chlorophyll), 0.0);
    const double sediment = s.sediment + extra;
    double weight[3] = {}, transmit[3] = {}, back[3] = {};
    for (int i = 0; i < kBands; ++i)
    {
        const double l = 400.0 + 10.0 * i;
        const double a = kPureWater[i] + (c > 0.0 ? 0.0654 * std::pow(c, 0.728) * kPhytoShape[i] : 0.0) +
                         s.cdom * std::exp(-0.015 * (l - 440.0)) +
                         0.031 * sediment * std::exp(-0.0123 * (l - 443.0));
        const double bw = 0.00288 * std::pow(l / 500.0, -4.32);
        const double bp = c > 0.0 ? 0.30 * std::pow(c, 0.62) * (550.0 / l) : 0.0;
        const double ratio = 0.002 + 0.01 * (0.5 - 0.25 * std::log10(std::max(c, 1e-3)));
        const double bm = 0.51 * sediment * std::pow(555.0 / l, 0.5);
        const double bb = 0.5 * bw + ratio * bp + 0.0183 * bm;
        double w[3];
        response(l, w);
        for (int k = 0; k < 3; ++k)
        {
            weight[k] += w[k];
            transmit[k] += w[k] * std::exp(-(a + bb) * kDepth);
            back[k] += w[k] * bb;
        }
    }
    for (int j = 0; j < 3; ++j)
    {
        kOut[j] = -std::log(transmit[j] / weight[j]) / kDepth;
        bbOut[j] = back[j] / weight[j];
    }
}

void compute(State& s)
{
    double k[3], bb[3], kSurf[3], bbSurf[3];
    channels(s, 0.0, k, bb);
    channels(s, kSurfReference, kSurf, bbSurf);
    for (int j = 0; j < 3; ++j)
    {
        s.medium[j] = static_cast<float>(k[j]);
        s.medium[4 + j] = static_cast<float>(bb[j]);
        s.medium[8 + j] = static_cast<float>((kSurf[j] - k[j]) / kSurfReference);
        s.medium[12 + j] = static_cast<float>((bbSurf[j] - bb[j]) / kSurfReference);
    }
    s.medium[3] = 0.0f;
    s.medium[7] = 0.0f;
    s.medium[11] = s.surf;
    s.medium[15] = 0.0f;
    s.dirty = false;
}

} // namespace

int rendererWaterType() { return g_state.type; }
void rendererSetWaterType(int type)
{
    if (type < 0 || type >= kPresetCount)
        return;
    g_state.type = type;
    g_state.chlorophyll = kPresets[type].chlorophyll;
    g_state.cdom = kPresets[type].cdom;
    g_state.sediment = kPresets[type].sediment;
    g_state.dirty = true;
}
int rendererWaterTypeCount() { return kPresetCount; }
const char* rendererWaterTypeName(int type)
{
    return type >= 0 && type < kPresetCount ? kPresets[type].name : "custom";
}

float rendererWaterConstituent(int which)
{
    return which == 0 ? g_state.chlorophyll : (which == 1 ? g_state.cdom : g_state.sediment);
}
void rendererSetWaterConstituent(int which, float value)
{
    float& slot = which == 0 ? g_state.chlorophyll : (which == 1 ? g_state.cdom : g_state.sediment);
    if (slot == value)
        return;
    slot = value;
    g_state.type = -1;
    g_state.dirty = true;
}

float rendererWaterSurfSediment() { return g_state.surf; }
void rendererSetWaterSurfSediment(float value)
{
    g_state.surf = value;
    g_state.medium[11] = value;
}

const float* rendererWaterMedium()
{
    if (g_state.dirty)
        compute(g_state);
    return g_state.medium;
}

void rendererWaterMediumFrame()
{
    stateManagerSetWaterMedium(rendererWaterMedium());
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

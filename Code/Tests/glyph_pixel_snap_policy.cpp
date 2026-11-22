// [CHOSEN, not recovered] Content-free checks of the whole-pixel menu glyph placement
// (GeomMisc/FontSystem/GlyphPixelSnap.h, issue #45). Synthetic font-unit lefts stand in for the
// retail layout's cells; no font, atlas or renderer is modelled.
#include "../BF2/GeomMisc/FontSystem/GlyphPixelSnap.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace gs = dice::hfe::geom::glyphsnap;
namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char* message, float scale = 0.0f)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL (scale %g): %s\n", scale, message); }
}

// Two words: "AAAAA" with equal 7-unit steps, a space, then steps 6,7,6,7,6.
const float lefts[] = { 0, 7, 14, 21, 28, 33, 38, 44, 51, 57, 64, 70 };
const unsigned char breaks[] = { 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
const unsigned n = sizeof(lefts) / sizeof(lefts[0]);

void expect(float scale, const int (&expected)[n])
{
    int out[n];
    gs::snapLefts(lefts, breaks, n, scale, out);
    bool same = true;
    for (unsigned i = 0; i < n; ++i)
        same = same && out[i] == expected[i];
    check(same, "pinned pixel lefts", scale);
}

// Equal font-unit steps inside a word get equal pixel steps unless the exact step is a half pixel
// (such a tie may take back word drift); glyphs stay within 2.5 px of their retail positions.
void properties(float scale)
{
    int out[n];
    gs::snapLefts(lefts, breaks, n, scale, out);
    std::map<int, std::set<int> > steps;
    for (unsigned i = 1; i < n; ++i)
    {
        if (breaks[i] || breaks[i - 1])
            continue;
        const double exact = (lefts[i] - lefts[i - 1]) * static_cast<double>(scale);
        const double off = std::fabs(exact - std::floor(exact + 0.5));
        const int pixels = out[i] - out[i - 1];
        check(std::fabs(pixels - exact) <= 0.5 + 1e-9,
              "a step stays within half a pixel of its exact width", scale);
        if (off < gs::kTieMargin - gs::kTieEpsilon)
            steps[static_cast<int>(lefts[i] - lefts[i - 1])].insert(pixels);
    }
    for (std::map<int, std::set<int> >::const_iterator it = steps.begin(); it != steps.end(); ++it)
        check(it->second.size() == 1, "equal font-unit steps give equal pixel steps", scale);
    for (unsigned i = 0; i < n; ++i)
        if (!breaks[i])
            check(std::fabs(out[i] - lefts[i] * static_cast<double>(scale)) <= 2.5,
                  "a glyph stays within 2.5 px of its retail position", scale);
}
}

int main()
{
    // Scale 1: the caller keeps the shipped arithmetic; the helpers agree with it exactly.
    {
        int out[n];
        gs::snapLefts(lefts, breaks, n, 1.0f, out);
        bool identity = true;
        for (unsigned i = 0; i < n; ++i)
            identity = identity && out[i] == static_cast<int>(lefts[i]);
        check(identity, "scale 1 keeps the retail lefts", 1.0f);
        check(gs::quadEdge(7, 1.0f) == 7.0f - 0.5f, "scale 1 quad edge is the shipped left - 0.5", 1.0f);
        check(gs::quadEdge(-3, 1.0f) == -3.0f - 0.5f, "scale 1 quad edge, negative", 1.0f);
    }

    // 1066/1024: a 1280x800 panel's 4:3 menu. Every 7 is 7 and every 6 is 6 (retail's
    // fractional placement lands these 7-unit steps at 7 or 8 pixels depending on the phase).
    { const int e[n] = { 1, 8, 15, 22, 29, 34, 40, 46, 53, 59, 66, 72 }; expect(1066.0f / 1024.0f, e); }
    // 1.25 (1280x1024 retail, 1600x1200 4:3): 8.75 -> 9 throughout; the 7.5 ties share the drift.
    { const int e[n] = { 0, 9, 18, 27, 36, 41, 48, 55, 64, 71, 80, 88 }; expect(1.25f, e); }
    { const int e[n] = { 0, 10, 20, 31, 42, 50, 57, 66, 76, 85, 96, 105 }; expect(1.5f, e); }
    // 2400/1024: a Retina display's 4:3 menu (the HiDPI atlas samples the same quads).
    { const int e[n] = { 1, 17, 33, 49, 65, 77, 90, 104, 120, 134, 150, 164 }; expect(2400.0f / 1024.0f, e); }

    const float scales[] = { 800.0f / 640.0f, 1066.0f / 1024.0f, 1.1f, 1.25f, 1.5f, 1.875f,
                             2400.0f / 1024.0f, 2.0f, 3.0f };
    for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i)
        properties(scales[i]);

    // A quad edge maps back to half a pixel before its whole pixel.
    const float s = 1066.0f / 1024.0f;
    check(std::fabs(gs::quadEdge(40, s) * s - 39.5f) < 1e-4f, "quad edge lands at pixel - 0.5", s);

    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

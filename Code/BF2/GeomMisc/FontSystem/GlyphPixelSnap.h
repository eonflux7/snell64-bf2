#pragma once

// [CHOSEN, not recovered] Whole-pixel menu glyph placement at fractional font scales, part of the
// 4:3 menu viewport deviation (issue #45, https://github.com/kr4uzi/OpenBF2/issues/45).
//
// Retail (RendDX9 0x100A4BF0 layout, 0x100A3DE0 draw) lays a run out in whole .dif font units and
// scales the glyph quads by the font scale (output width over 800 or 1024) with a linear filter.
// At a scale other than 1 the quads land at fractional pixels: equal letter pairs get different
// pixel gaps and alternate between sharp and blurred stems. The 4:3 menu viewport makes such
// scales the common case (1066/1024 on a 1280x800 panel, 2400/1024 on a Retina display).
//
// SwiffFont.cpp keeps retail's layout (pen, kerning, cells, measuring, line breaking, alignment)
// and only replaces the final quad position when the scale is not 1:
// - inside a word (a run of drawn glyphs between spaces) each step from one glyph to the next is
//   the scaled font-unit step rounded to whole pixels, so equal font-unit steps get equal pixel
//   steps and every glyph samples its atlas at the same sub-pixel phase;
// - of the drift this leaves against the word's scaled retail width, a pixel is taken back only
//   from exact half-pixel steps (at 1.25 or 1.5), which round either way equally well, so no step
//   is more than half a pixel from its exact value;
// - the word is then centred on its scaled retail extent, rounded to a whole pixel, so what drift
//   remains is shared by the spaces on both sides. With the shipped StandardText_13 briefing text
//   no glyph lands more than 2.5 pixels from its retail position at 1.041, 1.25, 1.5 or 2.34;
// - the quad's top is the scaled glyph top rounded, and each quad starts half a pixel before its
//   pixel (the D3D9 pixel-centre convention retail's -0.5 font-unit bias has at scale 1).
// Scale 1 (800 and 1024 wide menus) keeps the shipped arithmetic unchanged; the caller does not
// call these functions there. Content-free check: Code/Tests/glyph_pixel_snap_policy.cpp.

#include <cmath>
#include <vector>

namespace dice { namespace hfe { namespace geom { namespace glyphsnap {

inline int roundPixel(double value) { return static_cast<int>(std::floor(value + 0.5)); }

// A step whose own rounding is this far off (an exact half pixel) may take back word drift.
constexpr double kTieMargin = 0.5;
constexpr double kTieEpsilon = 1e-9;

// lefts[i]: the laid-out cell left in font units (whole numbers from the retail layout).
// breaks[i]: nonzero for a cell that separates words (a space, a nonbreaking space, a missing glyph).
// out[i]: the cell's pixel left relative to the run origin; break cells get their own rounded left.
inline void snapLefts(const float* lefts, const unsigned char* breaks, unsigned int n, float scale, int* out)
{
    const double s = scale;
    for (unsigned int i = 0; i < n; ++i)
        out[i] = roundPixel(static_cast<double>(lefts[i]) * s);
    std::vector<double> remainder;
    std::vector<int> step;
    unsigned int first = 0;
    while (first < n)
    {
        if (breaks[first]) { ++first; continue; }
        unsigned int last = first;
        while (last + 1 < n && !breaks[last + 1])
            ++last;
        const unsigned int steps = last - first;
        remainder.assign(steps, 0.0);
        step.assign(steps, 0);
        int total = 0;
        for (unsigned int k = 0; k < steps; ++k)
        {
            const double exact = (static_cast<double>(lefts[first + k + 1]) -
                                  static_cast<double>(lefts[first + k])) * s;
            step[k] = roundPixel(exact);
            remainder[k] = exact - step[k];      // > 0: rounded down, < 0: rounded up
            total += step[k];
        }
        int drift = (out[last] - out[first]) - total;   // pixels to give back to the word
        while (drift != 0)
        {
            const double sign = drift > 0 ? 1.0 : -1.0;
            int best = -1;
            for (unsigned int k = 0; k < steps; ++k)
            {
                const double r = remainder[k] * sign;
                if (r >= kTieMargin - kTieEpsilon && (best < 0 || r > remainder[best] * sign))
                    best = static_cast<int>(k);
            }
            if (best < 0)
                break;
            step[best] += drift > 0 ? 1 : -1;
            remainder[best] -= sign;              // now on the other side: never picked again
            drift += drift > 0 ? -1 : 1;
        }
        int width = 0;
        for (unsigned int k = 0; k < steps; ++k)
            width += step[k];
        const double exactWidth = (static_cast<double>(lefts[last]) -
                                   static_cast<double>(lefts[first])) * s;
        int x = roundPixel(static_cast<double>(lefts[first]) * s + (exactWidth - width) * 0.5);
        out[first] = x;
        for (unsigned int k = 0; k < steps; ++k)
        {
            x += step[k];
            out[first + k + 1] = x;
        }
        first = last + 1;
    }
}

// The quad's left or top edge in font units for a whole pixel: half a pixel before it, divided
// back by the scale so the shipped vertex transform (which multiplies by the scale) places it.
inline float quadEdge(int pixel, float scale)
{
    return static_cast<float>((static_cast<double>(pixel) - 0.5) / static_cast<double>(scale));
}

} } } }

#pragma once

#include <cstdint>

namespace dice { namespace hfe { namespace ui {

// [CHOSEN, not recovered] The Flash menus occupy a centered 4:3 rectangle (#45).
// Coordinates and dimensions are in drawable pixels. The right and bottom edges are exclusive.
struct MenuViewport
{
    unsigned x, y, width, height;

    static MenuViewport fit(unsigned outputWidth, unsigned outputHeight)
    {
        unsigned w = outputWidth;
        unsigned h = outputHeight;
        if (std::uint64_t(w) * 3 > std::uint64_t(h) * 4)
            w = static_cast<unsigned>(std::uint64_t(h) * 4 / 3);
        else
            h = static_cast<unsigned>(std::uint64_t(w) * 3 / 4);
        return {(outputWidth - w) / 2, (outputHeight - h) / 2, w, h};
    }

    // [CHOSEN, not recovered] The whole-number factor for UI drawn from bitmaps with no vector
    // source - the console's bitmap font, the menu's .ani cursors - on high-resolution outputs: the
    // fitted viewport's height over the 600 lines the HUD and menus are authored for, rounded down,
    // at least 1 (1 below 1200 lines, 2 at 1200-1799, 3 at 1800-2399). Rounding down keeps them
    // no larger against the UI than in the 800x600 menu (owner check 2026-11-20); whole numbers
    // keep each bitmap pixel a uniform block.
    static unsigned bitmapScale(unsigned outputWidth, unsigned outputHeight)
    {
        const unsigned k = fit(outputWidth, outputHeight).height / 600;
        return k ? k : 1;
    }
};

} } }

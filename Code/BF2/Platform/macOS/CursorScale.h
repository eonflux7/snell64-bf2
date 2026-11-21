// [CHOSEN, not recovered] macOS port: the size at which the Cocoa host shows the menu's .ani
// cursors (CocoaHost.mm, customCursor). Plain C++ without AppKit, so a headless check can pin its
// values. The host hands over what it holds, the game view's height in points and the back
// buffer's size in pixels, and the whole rule is applied here.
#ifndef PLATFORM_MACOS_CURSORSCALE_H
#define PLATFORM_MACOS_CURSORSCALE_H

#include "../../Menu/MenuViewport.h"     // [CHOSEN] the HiDPI cursor factor (bitmapScale)

namespace dice { namespace hfe { namespace macos {

// Back-buffer pixels per cursor pixel for a back buffer of width x height pixels:
// MenuViewport::bitmapScale, which is 1 below 1200 lines of the fitted 4:3 viewport (retail shows
// the cursor at one back-buffer pixel per cursor pixel), 2 from 1200 lines and 3 from 1800. A
// width or height that is not positive counts as 0, for which the factor is 1.
inline unsigned cursorBitmapScale(int backBufferWidth, int backBufferHeight)
{
    return ui::MenuViewport::bitmapScale(static_cast<unsigned>(backBufferWidth > 0 ? backBufferWidth : 0),
                                         static_cast<unsigned>(backBufferHeight > 0 ? backBufferHeight : 0));
}

// The rule in its two derived values. A cursor pixel covers `bitmapScale` back-buffer pixels, and
// a back-buffer pixel covers `pointsPerBackBufferPixel` points: the game view's height in points
// over the back buffer's height in pixels (1 for a window at one point per pixel, 0.5625 for a
// 2560x1600 full-screen back buffer on a 1440x900-point desktop).
//
// The product is kept between 0.5 and 4 points, 16 to 128 points for the 32-pixel cursors. Both
// bounds apply to the product. Bounding the view's share first and multiplying afterwards made
// the cursor larger than `bitmapScale` back-buffer pixels wherever that share is below 0.5: a
// windowed back buffer shrunk to fit the screen, a full-screen mode larger than the desktop's
// backing size (share 0.43 with factor 3: 1.5 points instead of 1.29).
inline double cursorPointsWithinBounds(double pointsPerBackBufferPixel, unsigned bitmapScale)
{
    const double points = pointsPerBackBufferPixel * (bitmapScale ? bitmapScale : 1u);
    return points < 0.5 ? 0.5 : points > 4.0 ? 4.0 : points;
}

// Points on screen per cursor pixel for a game view that is `viewHeightPoints` points high and
// shows a back buffer of backBufferWidth x backBufferHeight pixels. These are the values the host
// holds, so its call only passes them through, and the factor and the view's share are worked out
// here. A 2560x1600 back buffer shown 900 points high gives 1.125 points (the size the owner
// approved). A view or a back buffer without height has no cursor size: the answer is 0, and the
// host shows the system arrow in that case.
inline double cursorPointsPerPixel(double viewHeightPoints, int backBufferWidth, int backBufferHeight)
{
    if (!(viewHeightPoints > 0.0) || backBufferHeight <= 0)
        return 0.0;
    return cursorPointsWithinBounds(viewHeightPoints / backBufferHeight,
                                    cursorBitmapScale(backBufferWidth, backBufferHeight));
}

} } }
#endif

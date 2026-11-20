// [CHOSEN, not recovered] macOS port: the size at which the Cocoa host shows the menu's .ani
// cursors (CocoaHost.mm, customCursor). Plain C++ without AppKit, so a headless check can pin its
// values.
#ifndef PLATFORM_MACOS_CURSORSCALE_H
#define PLATFORM_MACOS_CURSORSCALE_H

namespace dice { namespace hfe { namespace macos {

// Points on screen per cursor pixel. The cursor follows the game image: a cursor pixel covers
// `bitmapScale` back-buffer pixels (MenuViewport::bitmapScale: 1 below 1200 lines, where retail
// shows the cursor at one back-buffer pixel per cursor pixel; 2 from 1200 lines, 3 from 1800),
// and a back-buffer pixel covers `pointsPerBackBufferPixel` points: the game view's height in
// points over the back buffer's height in pixels (1 for a window at one point per pixel, 0.5625
// for a 2560x1600 full-screen back buffer on a 1440x900-point desktop).
//
// The product is kept between 0.5 and 4 points, 16 to 128 points for the 32-pixel cursors. Both
// bounds apply to the product. Bounding the view's share first and multiplying afterwards made
// the cursor larger than `bitmapScale` back-buffer pixels wherever that share is below 0.5: a
// windowed back buffer shrunk to fit the screen, a full-screen mode larger than the desktop's
// backing size (share 0.43 with factor 3: 1.5 points instead of 1.29).
inline double cursorPointsPerPixel(double pointsPerBackBufferPixel, unsigned bitmapScale)
{
    const double points = pointsPerBackBufferPixel * (bitmapScale ? bitmapScale : 1u);
    return points < 0.5 ? 0.5 : points > 4.0 ? 4.0 : points;
}

} } }
#endif

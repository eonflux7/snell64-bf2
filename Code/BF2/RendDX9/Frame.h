// [RENDER] The frame module: the scene's pass sequence, from the post-production prepare to the
// HUD (docs/lanes/render.md, Frame). Not part of retail BF2, whose renderFrame carried
// the passes inline; Renderer::renderFrame (BF2Render.cpp) keeps the per-frame updates before the
// scene, the overlays after it, and Present.
#pragma once

struct IDirect3DSurface9;

namespace dice {
namespace hfe {
namespace dx9rend {

// What the frame reads from the Renderer, taken once per frame by renderFrame.
struct FrameInputs
{
    double time;               // the frame's time argument
    float  timeF;
    double frameTime;          // the time step, accumulated into GlobalTime and the water clock

    // The renderer's draw switches (console `renderer.draw*`).
    bool world;
    bool particles;
    bool decals;
    bool roads;
    bool terrain;
    bool undergrowth;
    bool overgrowth;
    bool sky;
    bool skyPass;
    bool postProduction;
    bool postProductionNeedsActive;   // post-production also asks the framework's active()

    int   clearDepthBeforeFirstPerson;   // a depth-only clear before the first-person passes
    void* objectDrawer;
    void* weatherManager;
    void* rainManager;
};

// Draws the scene into the scene target, from the post-production prepare through the
// post-production finish, and leaves the back buffer bound for the HUD. False when the device
// scene could not begin; nothing was drawn. With multisampling it unbinds the depth-stencil for
// the overlays and returns it, referenced, in overlayDepthStencil (null otherwise); the caller
// binds it back after the overlays and releases it.
bool frameRender(const FrameInputs& in, IDirect3DSurface9*& overlayDepthStencil);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

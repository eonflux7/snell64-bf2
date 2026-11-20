// [TOOLS] The view tool's shots (docs/lanes/tools.md#1-views), renderer side. Not part of retail
// BF2. While no shot is requested, each capture point returns after one test.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

class IRendererTools;

IRendererTools* toolsInterface();   // what Renderer::queryInterface(IID_IRendererTools) answers

// The two capture points. Before the HUD: in the frame's tail, after the composite and the sky's
// second pass, before the HudManager paints. After the HUD: in endScene, after EndScene (the debug
// menu and console draw later, at Present).
void toolsCaptureBeforeHud();
void toolsCaptureAfterHud();

} // namespace dx9rend
} // namespace hfe
} // namespace dice

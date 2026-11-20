// [UI] The fork's in-game UI on Dear ImGui (docs/lanes/ui.md). Not part of retail BF2.
// While nothing is shown, every hook below returns after one test.
#pragma once

struct IDirect3DSwapChain9;

namespace dice {
namespace hfe {
namespace dx9rend {

class IRendererUi;

// Before Present: creates the layer on first use, then draws the UI onto the back buffer when
// something is shown.
void uiLayerPresent(IDirect3DSwapChain9* chain);
void uiLayerPresented();     // after Present returns: the overlay's frame timing
void uiLayerDeviceLost();    // before the device is reset: releases the UI's device objects
void uiLayerShutdown();      // before the device is released: the layer goes, the window is restored

IRendererUi* uiLayerInterface();   // what Renderer::queryInterface(IID_IRendererUi) answers

int  uiLayerGetMenuKey();          // renderer.uiMenuKey: the Win32 virtual-key code toggling the menu
void uiLayerSetMenuKey(int key);
float uiLayerGetConsoleHeight();   // renderer.uiConsoleHeight: the console window's share of the height
void  uiLayerSetConsoleHeight(float share);
bool uiLayerGetOverlay();          // renderer.uiOverlay: the performance overlay (UiDebug.cpp)
void uiLayerSetOverlay(bool on);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

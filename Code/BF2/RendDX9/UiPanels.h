// [UI] The panels the UI layer draws (docs/lanes/ui.md). Not part of retail BF2.
// Internal to RendDX9: UiLayer.cpp owns the ImGui context and calls these between NewFrame and
// Render; the hooks around Present run every frame and do nothing while nothing is shown.
#pragma once

#include <string>

namespace dice {
namespace hfe {
namespace dx9rend {

class IUiHost;

// UiDebug.cpp: the debug menu and the performance overlay.
void uiDebugSetHost(IUiHost* host);   // reads the settings and command files
IUiHost* uiDebugHost();
bool uiOverlayShown();
void uiDebugFrameStart();      // top of uiLayerPresent: frame timing while the overlay is shown
void uiDebugBeforePresent();   // just before Present
void uiDebugPresented();       // just after Present
void uiDebugDrawMenu();
void uiDebugDrawOverlay();
// Runs a line as if typed into the console and entered (the console's access rules, echo,
// history; rcon lines go to the server).
void uiRunTyped(const std::string& line);
// Rewrites <mod>/settings/snell64-ui.con with the UI's settings (not while it is being read).
void uiSaveSettings();

// UiConsole.cpp: the console window.
bool uiConsoleShown();               // the game's console is active
void uiConsoleDraw(bool menuOpen);   // while shown
void uiConsoleHidden();              // once when it stops being shown

} // namespace dx9rend
} // namespace hfe
} // namespace dice

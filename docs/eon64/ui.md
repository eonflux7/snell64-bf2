# In-game UI

The `ui` lane: in-game UI this fork owns, built on Dear ImGui (1.92.9, vendored in
`Code/BF2/External/imgui` with its DX9 and Win32 backends; BF2Editor already uses it). Rules:
[AGENTS.md](../../AGENTS.md#lanes).

- Fork UI replaces the retail UI it supersedes. Once a milestone is accepted, the retail path is
  deleted, not kept beside it.
- Hidden UI costs nothing: no ImGui frame, no draw, no input capture, no timer queries.
- The dedicated server never creates the layer. The console backend is unaffected by this lane.
- UI never owns a setting. Anything a widget changes is a console variable, so it can also be set
  from scripts, `BF2_DEVCMD_FILE` and config files, and saved.
- PR-specific content (rcon command lists, admin commands) is never compiled in; it is read from
  files outside this tree.

## Milestones

| Milestone | Commit scope | Status |
|---|---|---|
| 1. ImGui layer | `ui(imgui)` | Accepted (`71545137`, `f4a7fd65`, `6604b184`) |
| 2. Sun shadow tuning as console variables (lane `render`) | `render(csm)` | Accepted (`2e605f93`) |
| 3. Debug menu and performance overlay | `ui(debug)` | Accepted (`ae0ee355`, `d9086de0`, `4b10dea9`; tabs and console polish `134889d5`) |
| 4. Console frontend | `ui(console)` | Accepted (`cfc56237`); retail drawing deleted |

### 1. ImGui layer

`RendDX9/UiLayer.cpp`; the game image's side is `RendMisc/IRendererUi.h` and
`BF2Engine::filterGameInput`.

- **Owner:** one ImGui context in RendDX9, which owns the device, `Reset` and `Present`. It is
  created at the first Present; ImGui is compiled into RendDX9 (`Code/Build/renddx9_sources.txt`).
- **Drawing:** in `rendererPresent`, just before `Present`: after the frame, the HUD and the
  screenshot (`renderer.dumpScreen` captures at `EndScene`), so screenshots never show the UI. Its
  own `BeginScene`/`EndScene` on the back buffer, never the HDR scene target. The DX9 backend's
  state block restores render state; the layer restores the render target, depth surface and sRGB
  states itself, so StateManager's caches stay true. Nothing runs while nothing is shown.
- **Device:** the renderer's lost-device listener (`releaseRendererPathResources`) releases the
  backend's objects; they are re-created on the next draw. `rendererReleaseD3DDevice` shuts the
  layer down.
- **Input, window side:** the layer chains a window procedure in front of the game's. The menu key
  toggles the menu and never reaches the game. While the menu is open, ImGui gets every message,
  and mouse, key and character messages stop there (system keys still pass, so Alt+F4 works).
- **Input, game side:** `BF2.exe` asks the renderer singleton for `IRendererUi` through
  `IBase::queryInterface(IID_IRendererUi)`; no export changes, so the plugin ABI check is
  untouched. While `uiCapturesInput()` is true, `filterGameInput` and `filterPlayerInput` deliver
  nothing (as with the console) and the DirectInput mouse is released, so Windows delivers the
  cursor. The message pump (`BF2.cpp`) re-activates the input devices every frame; meanwhile it
  leaves the mouse out, as it does for the main menu, and takes it back once the menu closes.
- **Cursor:** ImGui draws its own; the game keeps the system cursor hidden.
- **Font:** ImGui's embedded scalable font, 16 px at 1080p, scaled with the back-buffer height
  along with the style's sizes.
- **Hotkey:** `renderer.uiMenuKey`, a Win32 virtual-key code, 45 (Insert) by default. F1-F8
  switch vehicle seats.
- **Not written:** no `imgui.ini` or log next to the game.

### 2. Sun shadow tuning (lane `render`)

The values render milestone 5 was accepted with became console variables with the same defaults,
tuned live from the Shadows tab. Each carries its range and a help line for the debug menu. They
are listed in [renderer-pipeline.md](renderer-pipeline.md#sun-shadows).

### 3. Debug menu and performance overlay

`RendDX9/UiDebug.cpp`. The game image tells the UI the mod directory, map and game mode through
`IUiHost` (`IRendererUi::uiSetHost`, from `BF2Engine`).

**Menu** (the menu key): a header with the frame rate, the map and an overlay switch, then tabs.

- **Commands:** a button per command of `<mod>/settings/eon64-commands.txt`, read when the game
  hands the UI its host and again with Reload. `[Section]` lines group them under a heading;
  `label = console line | help` makes a button with that tooltip; `{name}` or `{name=default}`
  in the line adds a text field for an argument (Enter in a field also runs it); `#` starts a
  comment. Commands without arguments sit in a grid of equal buttons, those with arguments one
  per row. A filter searches labels, lines and help. A button runs its line as if typed into the
  console and entered (the console's access rules, echo and history; `rcon` lines go to the
  server). The file is the owner's, outside this repository; without it the tab says where it is
  looked for and what it holds.
- **Shadows:** the `renderer.sunShadow*` variables.
- **Atmosphere** (render milestone 11, while in development): `renderer.atmosphere` and
  `renderer.skyGainBlend`, the fit against its targets, the atlas format's vertex fetch check, and
  the four tables drawn opaque and read as data (linear, no exposure). The same, as files:
  `tools.atmosphere` ([tools.md](tools.md#atmosphere)).
- **Renderer:** every other `renderer.*` set/get property, with a filter.
- **Settings:** the overlay, the console height, the menu key, and the paths of the two files.

Variable rows are generated from the console registry: a checkbox for `bool`, a slider for a
number with a range, otherwise a text field applied with Enter. A widget sets its variable through
the registry, as the console does. Reset appears while a value differs from the one the menu
first showed. The tooltip gives the help line (`rendererVariableHelp`), the console name, the
type, the range and the value.

**Saved:** `<mod>/settings/eon64-ui.con`, console lines run when the game hands the UI its host,
rewritten when `renderer.uiOverlay`, `renderer.uiConsoleHeight` or `renderer.uiMenuKey` changes.
Nothing else is written.

**Look:** ImGui's dark style on a slate background with one teal accent and rounded frames, sized
at 1080p and scaled with the back-buffer height.

**Overlay**, top right, while `renderer.uiOverlay` is on. It takes no input; the system cursor is
left alone.

- Frames per second and frame time (the last 30 frames); over the last 5 seconds the average,
  the 1 % low (the 99th percentile frame) and the worst frame.
- A 5-second graph of frame time (white), CPU time (blue) and GPU time (orange), scaled to hold
  the worst frame, with a line at 16.7 ms. CPU time is the frame without the time `Present`
  blocked; GPU time is the whole frame from the timestamp queries. The larger of the two names
  the side that bounds the frame.
- GPU time per pass (the spans of [renderer-pipeline.md](renderer-pipeline.md#gpu-pass-timers)), smoothed. The queries
  are those of `BF2_GPU_TIMERS`, issued only while the overlay is on (or the flag is set).
- Draw calls and primitives in the last frame, and draw calls per sun cascade.
- Process memory: private bytes and working set. VRAM is not shown: D3D9's estimate under DXVK
  is not a real figure.
- Back-buffer size and MSAA, map and game mode.
- Camera position, heading and pitch.
- Sun elevation and azimuth. Exposure is a constant in the shaders until auto-exposure (render
  milestone 13) gives the engine a value to show.

DXVK's HUD is off in the launch environment.

### 4. Console frontend

`RendDX9/UiConsole.cpp`. Replaces the retail drop-down console's drawing (`Renderer::slot_0x58`)
and typing with an ImGui window over the same backend: BF2.exe's `io::Console` and the console
object registry.

- **Shown** while the game's console is active; the game's console key still toggles it, and the
  character that key types never reaches the window. Top of the screen, full width;
  `renderer.uiConsoleHeight` sets its share of the height (0.2-0.9, 0.45 by default, saved).
- **Typing:** a line entered in the window runs as if typed into the retail console and entered
  (`setCurrentLine`, then Enter through `updateAsciiKey`), so access rules, echo, the console's
  own history, `.con` execution and `rcon` are unchanged. Lines hold 127 characters, as the
  console's buffer does. While the window is shown, BF2.exe leaves out the retail console's typing
  (`updateGameInput`; the character ring stays empty because the window takes the characters) and
  releases the mouse, as for the menu.
- **Completion:** while typing the first word, a list under the input line shows the registry's
  matching `object.function` names with their argument types or the property's type and range,
  and the help line for the fork's renderer variables. Tab completes as far as all matches agree,
  then steps through them with the current one highlighted (Shift+Tab steps back).
- **History:** Up and Down recall the window's own entered lines (200 kept).
- **Log:** the console's output history (`getLines`), scrollable, following new output while
  scrolled to the end; a filter shows only the lines containing its text. Lines entered in the
  window are teal, lines with error, unknown, failed, invalid or not found red, warnings amber.
- **Copy and paste:** Copy puts the lines shown on the clipboard; Ctrl+V pastes into the input.
- `renderer.drawConsole` gated only the retail drawing; the window does not read it.

Command parsing, `.con` scripts, rcon and Python's console calls do not change. The retail
console drawing (`Renderer::slot_0x58`) is deleted; the slot stays empty for the vtable layout.

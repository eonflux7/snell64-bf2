# `ui` milestones 5–11: the front-end menu on RmlUi

Proposed 2026-11-25. The `ui` lane's rules apply ([ui.md](../lanes/ui.md)); this document holds
the menu milestones' design and tasks, as a `render` milestone's document does; each milestone is a
heading with its `State:` line ([WRITING.md](../WRITING.md)). The owner's open questions on it are
Q-3 to Q-6 in [STATUS.md](../STATUS.md#open-questions-for-the-owner).

The goal: the
front end (main menu, in-game Escape menu, loading screen, end of round) drawn by
[RmlUi](https://github.com/mikke89/RmlUi) (MIT; HTML/CSS-like documents, data bindings), and Flash
gone: gameswf, `SwiffPlayer.dll`, `SwiffRender`, `mainmenu.swf` and `endofround.swf` no longer
loaded or built. The HUD (`hudBuilder`, the meme node graph) and the ImGui dev UI are out of scope
and stay as they are.

## What is there now

| Layer | Where | Size |
|---|---|---|
| Menu driver | `IMenuLogic` (`Menu/MainMenuLogic`, `Menu/BetweenRoundsLogic`), driven by `BF2Engine::renderMenu` and the `ECMenu` events in `Core/ModuleRegistry.cpp` | ~800 lines |
| Menu service | `SwiffHost` (`Menu/SwiffHost*.cpp`, `ISwiffHost`, ~199 virtuals): the menu event queue `renderMenu` drains (quit, join, host, start mission, parse parameters, disconnect, mod switch, demo, logout), messages, page state, end-of-round data, the command-line file; the game posts to it (`postMessage`, `setPage`, `setEndOfRoundInfo`, `setGameStarting`, ~20 call sites) and `MusicPlayer` and RendDX9 read it | ~2,900 lines |
| Script API | ~474 ActionScript natives under `dice.bf2.*` (`External/gameswf/SwiffPlayer/SwiffPlayer.cpp`): thin wrappers over `ISwiffHost` and the settings singletons (`VideoSettings`, `AudioSettings`, `ControlSettings`, `GeneralSettings`, `GameServerSettings`, profiles, server browser, player stats) | 11,300 lines |
| Player | gameswf, `base/`, Expat; built only for `SwiffPlayer.dll` | ~25k lines |
| Drawing | `Geom/SwiffRender.cpp` (67-slot `ISwiffRender`), `GeomMisc/FontSystem/SwiffFont.cpp`, `swiffmenu.fx`; menu background video through `GeomMisc/MenuRend/Video.cpp` (Bink) | ~5,700 lines |

Outside `Menu/`, only two `BF2Engine` sites reach the player itself (`getSwiffPlayer()`); everything
else talks to the service. That is the seam: the service stays, and the player and script API
underneath are replaced.

## Design

A mod's menu is content *and logic*: a retail-style menu movie carries its own script (PR's
does: its launcher handshake, login gates, its own options and pages, [below](#what-the-new-menu-must-keep)).
A menu whose logic is all C++ would let a mod restyle it but not extend it, and a mod's logic
cannot be compiled into this tree. So screens are documents plus scripts, and the engine provides
data and actions. Five layers, each depending only on the ones below, with RmlUi types confined
to the integration.

```
 content        <mod>/ui/menu/*.rml, *.rcss, *.lua  documents and their scripts, outside the code
 ------------------------------------------------------------------------------------------
 Ui/Script/     the menu API as a script module (`menu.*`)          C++17, RmlUi Lua plugin
 Ui/Models/     data models: settings, browser lists, profiles,     C++17, RmlUi
                maps, end of round
 Ui/Rml/        RmlUi integration: system, file, render, input      C++17, RmlUi
 ------------------------------------------------------------------------------------
 Menu/Api/      typed menu API: settings, profiles, games, browser  C++14, no UI types
 Menu/Host/     MenuHost: event queue, messages, state (ex-SwiffHost) C++14, no UI types
 ------------------------------------------------------------------------------------
 RendDX9        IUiDrawDevice: vertices, textures, scissor, video     POD across the image
```

- **`Menu/Host/` (the service).** `SwiffHost` renamed `MenuHost` and its Flash-only parts
  (render capability flags, the player's cursor and mode flags, `ISwiffRender` queries) moved out
  or deleted. `ISwiffHost` becomes `IMenuHost` with the members the game, the API and the tests
  use; the ~20 callers follow the rename. The singleton name changes with it (`"MenuHost"`). It
  knows nothing about how the menu is drawn and is tested without a renderer (the existing
  `Tests/SwiffHostStandIn.h` tests port over).
- **`Menu/Api/` (the menu API).** The behaviour of the `dice.bf2.*` natives as plain C++: one
  header per domain (`VideoOptions`, `AudioOptions`, `ControlOptions`, `GeneralOptions`,
  `Profiles`, `LocalGame`, `ServerBrowser`, `EndOfRound`, `Locale`, `Mods`), returning values and
  structs instead of writing into script objects. Settings that already have console variables
  are read and written through the console registry, as the debug menu does ("UI never owns a
  setting"); the rest call the settings singletons with the apply, cancel and save-profile rules the
  natives follow now. A native with no caller in the shipped movies, or for a dead service (online
  login, GameSpy, stats, leaderboards, unlocks, PunkBuster), is not carried over.
- **`Ui/Rml/` (the integration).** The only code that includes RmlUi headers, built as its own
  C++17 static library (the engine stays C++14). `SystemInterface`: the engine clock, the log,
  `TranslateString` through the lexicon (`$KEY` strings). `FileInterface`: the engine's file
  manager, so documents load from mod directories and zips. `RenderInterface`: forwards to
  `IUiDrawDevice`. Input: `GameInput` and window messages to `Context::Process*`, clipboard and
  cursor shapes. The context scales with the back-buffer height (1080p reference), as the ImGui
  layer does.
- **`Ui/Models/` (data).** The heavy lists and state as RmlUi data models
  (`DataModelConstructor`), bound by name in documents: the server lists (thousands of rows,
  filtered and sorted in C++), players of the selected server, profiles, the map list and modes,
  settings, the end-of-round scores. A model holds no game state: it reads `Menu/Api` each frame
  it is dirty.
- **`Ui/Script/` (logic).** `Menu/Api` exposed to scripts as one `menu` module, through RmlUi's
  Lua plugin: documents carry `<script>` and inline handlers (`onclick="menu.quit()"`), as the
  movies carried ActionScript. Generic helpers a mod's logic needs and the engine has no reason
  to know about: a key-value store kept in the profile (the movies' `getStorageString`), the
  command-line option file, opening a URL. Screens are files, so layout, look and logic change
  without a build; `menu.reload` reloads them in place. The engine's default menu is written the
  same way, so the script path is the one every menu uses.
- **`IUiDrawDevice` (RendDX9).** RmlUi's render calls in a small interface of POD types, so no
  RmlUi or STL types cross the image boundary: compile and draw indexed geometry, upload and
  release textures (RGBA from memory, or a texture by path through the engine's loader), a
  video texture (the Bink player behind `Video.cpp`, for menu backgrounds), transform, scissor,
  and later clip masks through the stencil. Draws onto the back buffer after the frame (never the
  HDR target), restoring render target, depth and sRGB states itself so StateManager's caches
  stay true, as `UiLayer.cpp` does. Released with the device's lost-device listener. The game
  image gets it through `IBase::queryInterface`, as with `IRendererUi`; no plugin export
  changes.
- **The driver.** One `RmlMenuLogic` implementing `IMenuLogic` for both the main menu and the
  between-rounds screens (the mode is a document set, not a class). `IMenuLogic` loses
  `getSwiffPlayer` and `getSwiffRender`, and `getSwiffHost` becomes `getMenuHost`; the two
  `BF2Engine` sites change with it. Hidden menu, no cost: no context update, no draw.
- **Fonts.** FreeType (vendored, pinned) with a TTF the owner picks (an OFL font can ship in the
  public tree). `SwiffFont` is deleted with Flash if nothing else uses it (`Geom/MeshTemplate.cpp`
  references it: to check).
- **Content.** Documents load from `<mod>/ui/menu/`, falling back to `mods/bf2/ui/menu/`. The
  engine's default menu is written from scratch and can be public; a PR-styled menu is PR-derived
  material and lives outside this tree.

### What the new menu must keep

From PR (its movies inventoried in the private workspace, 2026-11-25):

- **The server browser, whole:** internet, LAN, favourites, history and co-op lists; filters,
  sorting, refresh; the selected server's details (players, map image, community and sponsor
  logos, welcome message); password; connect to an address; auto-join with retries; adding to
  favourites. The engine side is `ServerBrowser`/`ServerBrowserManager` on GameSpy's
  server-browsing SDK; the internet list needs a master server and so is not tested by agents
  (offline rule): agents test LAN, favourites, history and direct queries against a local server
  on a port other than 16567; the internet list is on the owner's look.
- **Mod logic in the menu:** the launcher handshake is the movie's own script (it reads the
  command-line option file and writes back the reply), and the movie gates pages on the login
  state the launcher passes. In the new menu this is PR's script, in PR's private content, on the
  `menu` module's command-line, storage and gate primitives; the engine does not implement PR's
  protocol.
- Create local and co-op games (map list, mode, size), demos with bookmarks and downloads, the
  mods list, per-vehicle control maps (with the mod's own key-binding headers), the mod's own
  options pages.
- The command-line file (`BF2_CMDLINE_FILE`) is read at menu start and its parameters run (menu
  event 5), as [compat.md](../lanes/compat.md) records.
- `BF2_AUTOHOST` (now in `SwiffHost::update`) and `+warmupLevel`'s dropped menu events.
- Menu sounds and music (`MusicPlayer` reads the service), the lexicon's strings, local profiles
  (create, select, save on apply).
- The loading screen draws while the level loads (`ECMenu` 4 with the literal 1.0 delta).

The movies' actual use of the natives is inventoried first (task 5.1), from `SWF_AS_TRACE` runs
and from decompiling the movies in the private workspace; the inventory stays there, and only
the resulting engine API list comes here.

## Milestones

During development the Flash path stays selectable by `BF2_UI_MENU=flash|rml` (an environment
flag: the menu cannot change backend while it runs; listed in [dev.md](../lanes/dev.md)). Unlike the
lane's other milestones, the retail path cannot go screen by screen (it is one movie), so it is
deleted once, in milestone 11, after 7–10 are accepted.

### ui-5 Foundations

State: in progress.

Check: the test document draws at 1080p and after a device reset and a resize, input works, the
frame time of the menu against the Flash menu's.

#### ui-5.1 Inventory of the natives and screens

State: built (2026-11-25). The natives and screens the shipped movies use, kept in the private
workspace.

#### ui-5.2 Libraries vendored

State: planned. RmlUi, its Lua plugin, Lua and FreeType in `External/`, pinned, as C++17 static
libraries in CMake and the VM build.

#### ui-5.3 The draw device

State: planned. `IUiDrawDevice` in RendDX9.

#### ui-5.4 Integration and the menu driver

State: planned. `Ui/Rml/` integration and `RmlMenuLogic` behind `BF2_UI_MENU=rml`, drawing one test
document with text, images, the background video and a scripted button that quits.

### ui-6 Service boundary

State: proposed.

`SwiffHost` → `MenuHost`, `ISwiffHost` → `IMenuHost`, Flash-only members moved to the Flash side;
`Menu/Api/` for the domains `ui-7` to `ui-9` need, with tests in `Tests/`, and its `menu` script
module. The Flash menu keeps working on the renamed service (checked by playing through it). No
visible change.

### ui-7 Main menu and options

State: proposed.

The shell (main screen, quit, mod switch), local profiles, and options: video, audio, controls
(key binding), general. `menu.reload`, and `menu.show SCREEN` for the `tools` checks (agents drive
the menu by console, not by clicks).

### ui-8 Multiplayer

State: proposed.

The server browser (all five lists, filters, sorting, details, password, connect to an address,
auto-join, favourites and history), create a local or co-op game (map, mode, size), demos, the
command-line file and `BF2_AUTOHOST` through the new menu.

### ui-9 Between rounds and in game

State: proposed.

Loading screen (map image, briefing, progress), end of round (scores, the round's summary), the
in-game Escape menu (resume, options, disconnect), the messages the game posts (kicked, connection
lost, version mismatch).

### ui-10 PR's menu on the new stack

State: proposed.

In the private workspace, not this tree. PR's screens, look and logic (the launcher handshake,
login gates, its options pages, Mumble bindings) as documents and scripts in the overlay, on the
default menu's models and `menu` module. What it needs from the engine and finds missing goes into
`ui-6` to `ui-9` as generic primitives, never as PR code here. Check: a launch through PR's
launcher reaches the menu logged in and joins a local server.

### ui-11 Flash removed

State: proposed.

Deleted: gameswf, `base/`, Expat, `SwiffPlayer.dll` and its build target, `SwiffPlayerAbi/`,
`SwiffRender`, `SwiffFont` (if unused), `swiffmenu.fx` use, the Swiff tests, the `BF2_UI_MENU` flag
and the `IMenuLogic` Flash members. Packaging no longer ships or loads the `.swf` files.

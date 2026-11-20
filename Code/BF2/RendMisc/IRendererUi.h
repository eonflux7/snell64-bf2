// [UI] The fork's in-game UI, as the game image sees it (docs/lanes/ui.md). Not part of retail BF2.
//
// The UI lives in RendDX9, which owns the device and Present. BF2.exe owns input, so it asks the
// renderer singleton for this interface (IBase::queryInterface(IID_IRendererUi)) and, while the UI
// takes the input, releases the mouse and delivers no game input. BF2.exe answers what only the
// game knows through IUiHost. No export changes: the plugin ABI is untouched.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// Fork-owned interface id; outside the ranges retail's Ids.h uses.
const unsigned int IID_IRendererUi = 0xE6400001u;

// [TOOLS] The view tool's views as the Views tab shows them (docs/lanes/tools.md#1-views). Read
// only: the tab changes views through view.* console lines, as typed. Strings stay valid until the
// next uiViewsRefresh.
struct UiView
{
    const char* name;
    const char* note;       // empty when none
    float x, y, z;          // world metres, y up
    float yaw, pitch;       // degrees
    float fov;              // vertical degrees; valid when hasFov
    bool  hasFov;
    int   runCount;
};

// A camera path of the view file (tools milestone 3).
struct UiPath
{
    const char* name;
    const char* note;       // empty when none
    int   keyCount;
    float length;           // seconds: the last key's time
};

class IUiViewTool
{
public:
    // Rereads the loaded map's view file when it changed on disk (checked at most twice a second).
    virtual void uiViewsRefresh() = 0;
    virtual const char* uiViewsPath() const = 0;      // empty while no map is loaded
    virtual bool uiViewsFileExists() const = 0;
    virtual int uiViewCount() const = 0;
    virtual bool uiViewAt(int index, UiView& view) const = 0;
    // A run line of the view; undone is false for a command (not restored on leaving).
    virtual const char* uiViewRun(int view, int run, bool& undone) const = 0;
    virtual int uiSetCount() const = 0;
    virtual const char* uiSetName(int set) const = 0;
    virtual int uiSetViewCount(int set) const = 0;
    virtual const char* uiSetView(int set, int index) const = 0;
    virtual int uiViewProblemCount() const = 0;
    virtual const char* uiViewProblem(int index) const = 0;
    virtual bool uiViewActive() const = 0;            // the tool holds the camera
    virtual bool uiViewFlying() const = 0;
    virtual const char* uiHeldView() const = 0;       // the view held, empty when none
    virtual int uiPathCount() const = 0;
    virtual bool uiPathAt(int index, UiPath& path) const = 0;
    // A key of the path as its file line ("key 4 pos=... yaw=..."); valid until the next call.
    virtual const char* uiPathKey(int path, int key) const = 0;
    virtual const char* uiPlayingPath() const = 0;    // the path playing, empty when none
    virtual float uiPathTime() const = 0;             // seconds into the playing path

protected:
    ~IUiViewTool() {}
};

// What the game image tells the UI. Strings stay valid until the next call.
class IUiHost
{
public:
    virtual const char* uiModDirectory() const = 0;   // GSModDirectory, for example "mods/pr"
    virtual const char* uiMapName() const = 0;        // empty while no level is loaded
    virtual const char* uiGameMode() const = 0;
    virtual IUiViewTool* uiViewTool() = 0;            // [TOOLS] the view tool

protected:
    ~IUiHost() {}
};

class IRendererUi
{
public:
    // True while a UI window has the mouse and keyboard (the menu is open): no game input.
    virtual bool uiCapturesInput() const = 0;
    // True once the UI draws the console: the retail console's typing (updateGameInput and the
    // character ring) is left out; the console window types instead.
    virtual bool uiOwnsConsole() const = 0;
    // True while the menu or the console window is shown: the DirectInput mouse stays released.
    virtual bool uiWantsMouse() const = 0;
    // Once, before the first frame the game drives: the UI then reads its settings files.
    virtual void uiSetHost(IUiHost* host) = 0;

protected:
    ~IRendererUi() {}
};

} // namespace dx9rend
} // namespace hfe
} // namespace dice

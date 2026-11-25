// [TOOLS] The view tool (docs/lanes/tools.md#1-views): camera override and free fly, tools
// milestone 1. Not part of retail BF2; client image only.
//
// The tool takes the render camera in BF2Render::render after the game has set it and before the
// renderer culls, picks LODs, streams or sets up cascades, so all of those follow the override.
// Only the client's camera moves: no soldier is spawned and the server does not know.
//
// Free fly reads the keyboard and mouse directly, once per rendered frame, so it moves as smoothly
// as the frame rate and also on the spawn screen, whose control map has no movement. While it
// flies, the DirectInput mouse is released (as for the debug menu; the message pump keeps it
// released) and the cursor is hidden and re-centred each frame: its offset is the mouse look. The
// game's player input is cleared in BF2Engine's filter, so neither a soldier nor the spawn menu
// reacts. Keys: W A S D, E or Space up, Q or C down, Shift faster, Ctrl slower; the mouse wheel
// changes the FOV. Input is read only while the game window is in the foreground and neither the
// debug menu nor the console window is shown; meanwhile the cursor shows. The HUD and spawn
// menu are hidden through renderer.drawHud while the tool is active.
//
// Views (task 3): each map's view file, <GSModDirectory>/settings/snell64-views/<map>.txt (ViewFile),
// is reread whenever it changed on disk, so hand edits apply at the next view command. view.go
// holds the camera at a view and runs its `run` lines; a line that sets a console variable records
// the variable's value first, and leaving the view restores it. Lines that call commands are not
// undone. Free fly started from a view stops where it is and keeps holding the camera there; free
// fly started from the game's camera hands it back when it stops.
//
// Paths (milestone 3): `path NAME` blocks in the same file, keyframes added by path.key at the held
// camera. path.play takes the camera along the path in real time from its first rendered frame and
// hands it back at the end; path.stop, view.go, view.leave and free fly end it early (free fly goes
// on from where the path was).
//
// Benchmark (milestone 3): bench.run plays a path once to warm up and then the given number of
// times back to back, uncapped and without vsync, while the renderer records every frame
// (IRendererTools.h); a few frames after the last run, once the GPU times are in, it writes a CSV
// and a summary. Anything that ends the path early stops the benchmark without files.
//
// Camera convention: BF2's camera-to-world matrix (rows right, up, forward, position; y up),
// yaw and pitch in degrees as getRotationZXY reports them (pitch negative looking down), the FOV
// vertical in degrees.

#include "ViewTool.h"

#if !DEDICATED

#include "ViewFile.h"
#include "../IGame.h"
#include "../../Core/ConsoleObject.h"
#include "../../Core/ConsoleObjectMacro.h"
#include "../../Core/ConsoleObjects.h"
#include "../../Core/ClassManager.h"
#include "../../Core/SmartItf.h"
#include "../../IO/File/FileManager.h"
#include "../../Core/SettingsRepostitory.h"
#include "../../IO/Console/Console.h"
#include "../../IO/Input/Input.h"
#include "../../IO/Input/PlayerInput.h"
#include "../../Math/Mat4.h"
#include "../../RendMisc/IRenderView.h"
#include "../../RendMisc/IRendererTools.h"
#include "../../RendMisc/IRendererUi.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>    // after the engine headers: GetAsyncKeyState, the cursor

namespace dice {
namespace hfe {

namespace io { extern IBase* inputDeviceManager; }   // Core/Globals.cpp
bool uiWantsMouse();    // BF2Engine.cpp: the debug menu or the console window is shown

bool g_viewToolActive = false;
bool g_viewToolFlying = false;
bool g_viewToolShooting = false;
bool g_viewToolBenching = false;
bool g_viewToolSurveying = false;
bool g_viewToolUncapped = false;

namespace {

const char* const kViewToolConsoleTag = "tools.view";

const float kDegToRad = 3.14159265358979f / 180.0f;
const float kMinFov = 5.0f;
const float kMaxFov = 120.0f;
const float kFovStep = 2.0f;          // degrees per wheel notch

struct Camera
{
    float x, y, z;
    float yaw, pitch;   // degrees
    float fov;          // vertical, degrees
};

struct State
{
    bool  fly;
    bool  seedFromGame;     // take the game's camera at the next apply
    bool  seedFovFromGame;  // take only the game's FOV at the next apply (a view without fov=)
    bool  recentre;         // the next frame only centres the cursor
    bool  cursorHidden;
    Camera camera;
    std::string savedDrawHud;
    int   wheel;            // wheel notches since the last frame
    std::chrono::steady_clock::time_point lastStep;
    bool  haveLastStep;
    std::string view;       // the view held, empty when none (free fly from the game's camera)
    std::vector<std::pair<std::string, std::string> > restores;  // variable, value before the view
};

State g_state = { false, false, false, false, false, { 0, 0, 0, 0, 0, 60.0f }, std::string(), 0, {}, false,
                  std::string(), std::vector<std::pair<std::string, std::string> >() };

ViewFile g_views;

// A camera path playing (path.play, milestone 3): the keys as played, yaw unwrapped so each step
// takes the short way round and every key's FOV filled in.
struct PathPlay
{
    bool playing;
    bool started;           // the first frame has set the start time
    bool fovFromGame;       // no key has fov=: the game's FOV at the first frame, for all keys
    std::string name;
    std::vector<PathKey> keys;
    std::chrono::steady_clock::time_point start;
};

PathPlay g_play = { false, false, false, std::string(), std::vector<PathKey>(), {} };

// tools.* variables
float g_flySpeed = 20.0f;   // metres per second
float g_flyMouse = 0.15f;   // degrees per pixel of mouse movement
int   g_flyKey   = 0x77;    // VK_F8

std::string runConsole(const std::string& line)
{
    std::string out;
    if (io::mainConsole)
        io::mainConsole->executeLine(line, out, false, false, false, false, false);
    return out;
}

void hideHud()
{
    g_state.savedDrawHud = runConsole("renderer.drawHud");
    runConsole("renderer.drawHud 0");
}

std::string trimEnd(std::string value)
{
    while (!value.empty() && (value[value.size() - 1] == '\n' || value[value.size() - 1] == '\r'
                              || value[value.size() - 1] == ' '))
        value.erase(value.size() - 1);
    return value;
}

void restoreHud()
{
    const std::string value = trimEnd(g_state.savedDrawHud);
    runConsole("renderer.drawHud " + (value.empty() ? std::string("1") : value));
}

// The console variable a run line sets ("renderer.sunShadowDebug 1" sets renderer.sunShadowDebug),
// or empty when the line calls a command or only reads a variable.
std::string variableSetBy(const std::string& line)
{
    std::istringstream in(line);
    std::string name, value;
    if (!(in >> name) || !(in >> value) || !mainConsoleObjects)
        return std::string();
    const size_t dot = name.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size())
        return std::string();
    ConsoleObject* object =
        mainConsoleObjects->getConsoleObject(name.substr(0, dot), name.substr(dot + 1));
    return object && object->isSetGet() ? name : std::string();
}

// Runs a view's line; before a variable is first set, its value is recorded for undoRuns.
std::string runViewLine(const std::string& line)
{
    const std::string variable = variableSetBy(line);
    if (!variable.empty())
    {
        bool recorded = false;
        for (size_t i = 0; i < g_state.restores.size() && !recorded; ++i)
            recorded = g_state.restores[i].first == variable;
        if (!recorded)
            g_state.restores.push_back(std::make_pair(variable, trimEnd(runConsole(variable))));
    }
    return trimEnd(runConsole(line));
}

// Restores the variables the held view's run lines set, last first.
void undoRuns()
{
    for (size_t i = g_state.restores.size(); i-- > 0;)
        runConsole(g_state.restores[i].first + " " + g_state.restores[i].second);
    g_state.restores.clear();
}

// The window that takes the input: the foreground window, when it is this process's.
HWND gameWindow()
{
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (!window)
        return 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId() ? window : 0;
}

void startFlying()
{
    if (g_state.fly)
        return;
    g_state.fly = true;
    g_viewToolFlying = true;
    g_state.haveLastStep = false;
    g_state.recentre = true;
    g_state.wheel = 0;
    // Release the DirectInput mouse, as the debug menu does; the message pump keeps it released
    // while g_viewToolFlying and re-acquires it afterwards.
    io::IInputDevice* mouse =
        static_cast<io::IInputDeviceManager*>(io::inputDeviceManager)->getInputDevice(io::IDFMouse);
    if (mouse)
        mouse->deActivate();
}

// The cursor is hidden while free fly reads the mouse, and shown for the menu or the console.
void showCursor(bool show)
{
    if (show == !g_state.cursorHidden)
        return;
    g_state.cursorHidden = !show;
    ShowCursor(show ? TRUE : FALSE);
}

void stopFlying()
{
    if (!g_state.fly)
        return;
    g_state.fly = false;
    g_viewToolFlying = false;
    showCursor(true);
}

void enter()
{
    if (g_viewToolActive)
        return;
    g_viewToolActive = true;
    hideHud();
}

void leave()
{
    if (!g_viewToolActive)
        return;
    g_viewToolActive = false;
    g_play.playing = false;
    stopFlying();
    undoRuns();
    g_state.view.clear();
    restoreHud();
}

void cameraToMatrix(const Camera& c, Mat4& m)
{
    m = Mat4::getIdentity();
    setRotationZXY(m, 0.0f, c.pitch, c.yaw);
    m.m[12] = c.x;
    m.m[13] = c.y;
    m.m[14] = c.z;
}

void cameraFromView(dx9rend::IRenderView* view, Camera& c)
{
    const Mat4& m = *view->getViewMatrixPtr();      // the camera-to-world matrix the game set
    float roll = 0.0f;
    getRotationZXY(m, roll, c.pitch, c.yaw);
    c.x = m.m[12];
    c.y = m.m[13];
    c.z = m.m[14];
    c.fov = view->getFov() / kDegToRad;
}

void setFly(int on)
{
    if (on)
    {
        if (!g_viewToolActive)
            g_state.seedFromGame = true;
        g_play.playing = false;     // a path stops where it is, and free fly goes on from there
        startFlying();
        enter();
    }
    else if (!g_state.view.empty())
    {
        stopFlying();       // from a view: hold the camera where free fly left it
    }
    else
    {
        leave();            // from the game's camera: hand it back
    }
}

int getFly() { return g_state.fly ? 1 : 0; }

void viewLeave() { leave(); }

// <GSModDirectory>/settings/snell64-views/<map>.txt for the loaded map, or empty with a reason.
std::string viewFilePath(std::string& error)
{
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
    {
        error = "no map loaded";
        return std::string();
    }
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    std::string dir = settings
        ? settings->stringGet(std::string("GSModDirectory"), std::string("mods/bf2"))
        : std::string("mods/bf2");
    if (!dir.empty() && dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\')
        dir += '/';
    return dir + "settings/snell64-views/" + map + ".txt";
}

// Rereads the view file when it changed on disk or the map changed. False with a reason when no
// map is loaded.
bool refreshViews(std::string& error, bool force = false)
{
    const std::string path = viewFilePath(error);
    if (path.empty())
        return false;
    if (force || g_views.changedOnDisk(path))
        g_views.load(path);
    return true;
}

std::string problemsText()
{
    std::string text;
    for (size_t i = 0; i < g_views.problems().size(); ++i)
        text += "\n  " + g_views.problems()[i];
    return text;
}

std::string viewGo(const std::string& name)
{
    std::string error;
    if (!refreshViews(error))
        return "view.go: " + error;
    const ViewDef* def = g_views.find(name);
    if (!def)
        return "view.go: no view " + name + " in " + g_views.path();
    const ViewDef view = *def;      // run lines may reload the file

    undoRuns();                     // the previous view's variables
    stopFlying();
    g_play.playing = false;
    g_state.seedFromGame = false;
    g_state.camera.x = view.x;
    g_state.camera.y = view.y;
    g_state.camera.z = view.z;
    g_state.camera.yaw = view.yaw;
    g_state.camera.pitch = view.pitch;
    if (view.hasFov)
        g_state.camera.fov = view.fov;
    g_state.seedFovFromGame = !view.hasFov;
    enter();
    g_state.view = view.name;

    std::string text = "view " + view.name;
    if (!view.note.empty())
        text += " | " + view.note;
    for (size_t i = 0; i < view.runs.size(); ++i)
    {
        const std::string out = runViewLine(view.runs[i]);
        text += "\n  run " + view.runs[i] + (out.empty() ? std::string() : "  -> " + out);
    }
    return text;
}

// The camera the tool holds (free fly or a view) becomes view NAME; a note replaces the old one.
std::string viewSave(const std::string& name, const std::string& note = std::string())
{
    if (!g_viewToolActive)
        return "view.save: no camera to save; start free fly (view.fly 1) or go to a view first";
    if (!ViewFile::validName(name))
        return "view.save: a name has no spaces, '|', '#' or '='";
    std::string error;
    if (!refreshViews(error))
        return "view.save: " + error;
    const bool existed = g_views.find(name) != 0;
    const Camera& c = g_state.camera;
    g_views.saveView(name, c.x, c.y, c.z, c.yaw, c.pitch, c.fov, note);
    CreateDirectoryA(g_views.path().substr(0, g_views.path().find_last_of('/')).c_str(), 0);
    if (!g_views.write(error))
        return "view.save: " + error;
    return (existed ? "updated view " : "saved view ") + name + " in " + g_views.path();
}

std::string viewDelete(const std::string& name)
{
    std::string error;
    if (!refreshViews(error))
        return "view.delete: " + error;
    if (!g_views.deleteView(name))
        return "view.delete: no view " + name + " in " + g_views.path();
    if (!g_views.write(error))
        return "view.delete: " + error;
    return "deleted view " + name;
}

std::string viewList()
{
    std::string error;
    if (!refreshViews(error))
        return "view.list: " + error;
    std::ostringstream out;
    out << g_views.path() << (g_views.exists() ? "" : " (no file yet)");
    const std::vector<ViewDef>& views = g_views.views();
    for (size_t i = 0; i < views.size(); ++i)
    {
        const ViewDef& v = views[i];
        out << "\n" << (v.name == g_state.view ? "* " : "  ") << v.name << "  pos="
            << ViewFile::formatNumber(v.x, 2) << "," << ViewFile::formatNumber(v.y, 2) << ","
            << ViewFile::formatNumber(v.z, 2) << " yaw=" << ViewFile::formatNumber(v.yaw, 2)
            << " pitch=" << ViewFile::formatNumber(v.pitch, 2);
        if (v.hasFov)
            out << " fov=" << ViewFile::formatNumber(v.fov, 1);
        if (!v.note.empty())
            out << " | " << v.note;
        for (size_t r = 0; r < v.runs.size(); ++r)
            out << "\n      run " << v.runs[r]
                << (variableSetBy(v.runs[r]).empty() ? "  (command, not undone)" : "");
    }
    const std::vector<ViewSet>& sets = g_views.sets();
    for (size_t i = 0; i < sets.size(); ++i)
    {
        out << "\n  set " << sets[i].name << ":";
        for (size_t v = 0; v < sets[i].views.size(); ++v)
            out << " " << sets[i].views[v];
    }
    return out.str() + problemsText();
}

std::string viewReload()
{
    std::string error;
    if (!refreshViews(error, true))
        return "view.reload: " + error;
    std::ostringstream out;
    out << g_views.path() << ": " << g_views.views().size() << " views, " << g_views.sets().size()
        << " sets, " << g_views.paths().size() << " paths"
        << (g_views.exists() ? "" : " (no file yet)");
    return out.str() + problemsText();
}

// Writes the file after an edit; "" when it was written, else the reason.
std::string writeViews()
{
    std::string error;
    return g_views.write(error) ? std::string() : error;
}

// Moves view NAME one place up (-1, "up") or down (1, "down") in the file, and so in the list.
std::string viewMove(const std::string& name, const std::string& direction)
{
    const int delta = direction == "up" || direction == "-1" ? -1
                    : direction == "down" || direction == "1" ? 1 : 0;
    if (delta == 0)
        return "view.move: direction is up or down";
    std::string error;
    if (!refreshViews(error))
        return "view.move: " + error;
    if (!g_views.find(name))
        return "view.move: no view " + name + " in " + g_views.path();
    if (!g_views.moveView(name, delta))
        return "view.move: " + name + " is already " + (delta < 0 ? "first" : "last");
    error = writeViews();
    return error.empty() ? "moved view " + name + " " + (delta < 0 ? "up" : "down")
                         : "view.move: " + error;
}

// Replaces view NAME's note; no note removes it.
std::string viewNote(const std::string& name, const std::string& note = std::string())
{
    std::string error;
    if (!refreshViews(error))
        return "view.note: " + error;
    if (!g_views.setNote(name, note))
        return "view.note: no view " + name + " in " + g_views.path();
    error = writeViews();
    return error.empty() ? (note.empty() ? "removed the note of " : "set the note of ") + name
                         : "view.note: " + error;
}

// Set NAME becomes the views listed (one argument, quoted when there are several); no views
// removes the set.
std::string viewSet(const std::string& name, const std::string& list = std::string())
{
    if (!ViewFile::validName(name))
        return "view.set: a name has no spaces, '|', '#' or '='";
    std::string error;
    if (!refreshViews(error))
        return "view.set: " + error;
    std::vector<std::string> views;
    std::istringstream in(list);
    std::string view;
    while (in >> view)
    {
        if (!g_views.find(view))
            return "view.set: no view " + view + " in " + g_views.path();
        views.push_back(view);
    }
    bool existed = false;
    for (size_t i = 0; i < g_views.sets().size(); ++i)
        existed = existed || g_views.sets()[i].name == name;
    if (views.empty() && !existed)
        return "view.set: no set " + name;
    g_views.setSet(name, views);
    CreateDirectoryA(g_views.path().substr(0, g_views.path().find_last_of('/')).c_str(), 0);
    error = writeViews();
    if (!error.empty())
        return "view.set: " + error;
    return (views.empty() ? "removed set " : existed ? "updated set " : "added set ") + name;
}

// path.* (milestone 3): camera paths in the view file, played in real time.

std::string pathKeyLine(const PathKey& k)
{
    std::string line = "key " + ViewFile::formatNumber(k.time, 2) + " pos="
        + ViewFile::formatNumber(k.x, 2) + "," + ViewFile::formatNumber(k.y, 2) + ","
        + ViewFile::formatNumber(k.z, 2) + " yaw=" + ViewFile::formatNumber(k.yaw, 2) + " pitch="
        + ViewFile::formatNumber(k.pitch, 2);
    if (k.hasFov)
        line += " fov=" + ViewFile::formatNumber(k.fov, 1);
    return line;
}

// The held camera becomes a keyframe of path NAME at TIME seconds; without TIME, 0 for a new path
// and 5 s after the last key otherwise. A key at the same time is replaced.
std::string pathKey(const std::string& name, const std::string& timeText = std::string())
{
    if (!g_viewToolActive)
        return "path.key: no camera to save; start free fly (view.fly 1) or go to a view first";
    if (!ViewFile::validName(name))
        return "path.key: a name has no spaces, '|', '#' or '='";
    std::string error;
    if (!refreshViews(error))
        return "path.key: " + error;
    const ViewPath* path = g_views.findPath(name);
    float time = path && !path->keys.empty() ? path->keys.back().time + 5.0f : 0.0f;
    if (!timeText.empty())
    {
        char* end = 0;
        time = static_cast<float>(std::strtod(timeText.c_str(), &end));
        if (!end || *end != '\0' || time < 0.0f)
            return "path.key: TIME is seconds from the path's start, 0 or more";
    }
    const Camera& c = g_state.camera;
    // Right after view.go to a view without fov=, the game's FOV is not read until the next frame;
    // the key then goes without fov= and takes the previous key's when played.
    g_views.saveKey(name, time, c.x, c.y, c.z, c.yaw, c.pitch,
                    g_state.seedFovFromGame ? 0.0f : c.fov);
    CreateDirectoryA(g_views.path().substr(0, g_views.path().find_last_of('/')).c_str(), 0);
    error = writeViews();
    if (!error.empty())
        return "path.key: " + error;
    path = g_views.findPath(name);
    std::ostringstream out;
    out << "path " << name << ": key at " << ViewFile::formatNumber(time, 2) << " s, "
        << (path ? path->keys.size() : 0) << " keys";
    return out.str();
}

std::string pathList()
{
    std::string error;
    if (!refreshViews(error))
        return "path.list: " + error;
    std::ostringstream out;
    out << g_views.path() << (g_views.exists() ? "" : " (no file yet)");
    const std::vector<ViewPath>& paths = g_views.paths();
    for (size_t i = 0; i < paths.size(); ++i)
    {
        const ViewPath& p = paths[i];
        out << "\n" << (g_play.playing && p.name == g_play.name ? "* " : "  ") << p.name << "  "
            << p.keys.size() << " keys, "
            << ViewFile::formatNumber(p.keys.empty() ? 0.0f : p.keys.back().time, 2) << " s";
        if (!p.note.empty())
            out << " | " << p.note;
        for (size_t k = 0; k < p.keys.size(); ++k)
            out << "\n      " << pathKeyLine(p.keys[k]);
    }
    if (paths.empty())
        out << "\n  no paths";
    return out.str() + problemsText();
}

std::string pathPlay(const std::string& name)
{
    if (g_viewToolBenching)
        return "path.play: bench.run is running; bench.stop ends it";
    std::string error;
    if (!refreshViews(error))
        return "path.play: " + error;
    const ViewPath* path = g_views.findPath(name);
    if (!path)
        return "path.play: no path " + name + " in " + g_views.path();
    if (path->keys.size() < 2)
        return "path.play: path " + name + " needs at least two keys";

    std::vector<PathKey> keys = path->keys;
    // Yaw the short way round from each key to the next.
    for (size_t k = 1; k < keys.size(); ++k)
    {
        while (keys[k].yaw - keys[k - 1].yaw > 180.0f)
            keys[k].yaw -= 360.0f;
        while (keys[k].yaw - keys[k - 1].yaw < -180.0f)
            keys[k].yaw += 360.0f;
    }
    // A key without fov= takes the previous key's; keys before the first fov= take that one.
    size_t first = 0;
    while (first < keys.size() && !keys[first].hasFov)
        ++first;
    for (size_t k = 0; first < keys.size() && k < keys.size(); ++k)
        if (!keys[k].hasFov)
            keys[k].fov = k < first ? keys[first].fov : keys[k - 1].fov;

    undoRuns();
    stopFlying();
    g_state.view.clear();
    g_state.seedFromGame = false;
    g_state.seedFovFromGame = false;
    g_play.keys.swap(keys);
    g_play.name = name;
    g_play.fovFromGame = first == g_play.keys.size();
    g_play.started = false;
    g_play.playing = true;
    enter();
    std::ostringstream out;
    out << "path " << name << ": " << g_play.keys.size() << " keys, "
        << ViewFile::formatNumber(g_play.keys.back().time, 2) << " s";
    return out.str();
}

// Stops a playing path and hands the camera back to the game.
void pathStop()
{
    if (g_play.playing)
        leave();
}

// The camera at time t (seconds) on the playing path: a cubic Hermite through the keys with
// Catmull-Rom tangents for uneven key times, (p[i+1] - p[i-1]) / (t[i+1] - t[i-1]), one-sided at
// the ends. Position, yaw, pitch and FOV each follow the same curve.
void samplePath(float t, Camera& c)
{
    const std::vector<PathKey>& k = g_play.keys;
    const size_t n = k.size();
    size_t i = 0;
    while (i + 2 < n && t >= k[i + 1].time)
        ++i;
    const float h = k[i + 1].time - k[i].time;
    float s = (t - k[i].time) / h;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    const size_t a0 = i == 0 ? 0 : i - 1;       // the tangent at key i spans keys a0 .. i + 1
    const size_t b1 = i + 2 < n ? i + 2 : i + 1; // the tangent at key i + 1 spans i .. b1
    const float ta = (k[i + 1].time - k[a0].time);
    const float tb = (k[b1].time - k[i].time);

    float out[6];
    for (int ch = 0; ch < 6; ++ch)
    {
        float v[4];
        const PathKey* keys[4] = { &k[a0], &k[i], &k[i + 1], &k[b1] };
        for (int j = 0; j < 4; ++j)
        {
            const PathKey& key = *keys[j];
            const float values[6] = { key.x, key.y, key.z, key.yaw, key.pitch, key.fov };
            v[j] = values[ch];
        }
        const float m0 = (v[2] - v[0]) / ta;
        const float m1 = (v[3] - v[1]) / tb;
        out[ch] = h00 * v[1] + h10 * h * m0 + h01 * v[2] + h11 * h * m1;
    }
    c.x = out[0];
    c.y = out[1];
    c.z = out[2];
    c.yaw = out[3];
    while (c.yaw > 180.0f)   c.yaw -= 360.0f;
    while (c.yaw <= -180.0f) c.yaw += 360.0f;
    c.pitch = out[4] > 89.0f ? 89.0f : out[4] < -89.0f ? -89.0f : out[4];
    if (!g_play.fovFromGame)
        c.fov = out[5] < kMinFov ? kMinFov : out[5] > kMaxFov ? kMaxFov : out[5];
}

bool benchPathEnded();

// One frame of a playing path; false when it has ended (the camera is handed back).
bool pathStep(dx9rend::IRenderView* view)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!g_play.started)
    {
        g_play.started = true;
        g_play.start = now;
        if (g_play.fovFromGame)
            g_state.camera.fov = view->getFov() / kDegToRad;
    }
    float t = std::chrono::duration<float>(now - g_play.start).count();
    if (t >= g_play.keys.back().time)
    {
        if (!benchPathEnded())
        {
            leave();
            return false;
        }
        g_play.start = now;     // the benchmark's next pass starts with this frame
        t = 0.0f;
    }
    samplePath(t, g_state.camera);
    return true;
}

// view.shot (task 5): the request waits for the next frame's hook, which asks the renderer for a
// shot of that frame (IRendererTools.h); a later hook collects the result. tools.shotHud shows
// the HUD for the shot, also while a view hides it, and hides it again afterwards.
struct Shot
{
    bool        queued;     // waiting for the hook to request it
    bool        requested;  // the renderer has the request
    bool        hud;
    bool        hudShown;   // the HUD was shown for this shot and is hidden again afterwards
    int         frames;     // hooks since queued or requested
    std::string png;
    std::string jpg;
    std::string last;       // the last shot's result, for view.lastShot
};

Shot g_shot = { false, false, false, false, 0, std::string(), std::string(), std::string() };

std::string g_outputDir("snell64-runs");   // under the game's working directory
int g_shotHud = 0;
int g_shotWidth = 960;      // the JPEG's width

const int kShotHudFrames = 2;       // frames the HUD draws before its shot
const int kShotTimeoutFrames = 600; // frames a request may wait for the capture point (it waits
                                    // out the renderer's rebuilds: ToolsCapture.cpp)

dx9rend::IRendererTools* rendererTools()
{
    static dx9rend::IRendererTools* tools = 0;
    if (!tools && classManager)
    {
        IBase* renderer = classManager->getSingleton("Renderer");
        if (renderer)
            tools = static_cast<dx9rend::IRendererTools*>(
                renderer->queryInterface(dx9rend::IID_IRendererTools));
    }
    return tools;
}

// tools.outputDir (@HOME@ allowed) made absolute, with backslashes and no trailing separator.
std::string outputDirectory()
{
    std::string dir = g_outputDir;
    static const std::string home("@HOME@");
    if (dir.compare(0, home.size(), home) == 0 && io::fileManager)
        dir = io::fileManager->getHomeSubstitution() + dir.substr(home.size());
    for (size_t i = 0; i < dir.size(); ++i)
        if (dir[i] == '/')
            dir[i] = '\\';
    char full[MAX_PATH];
    const DWORD n = GetFullPathNameA(dir.c_str(), MAX_PATH, full, 0);
    if (n > 0 && n < MAX_PATH)
        dir = full;
    while (dir.size() > 3 && dir[dir.size() - 1] == '\\')
        dir.erase(dir.size() - 1);
    return dir;
}

// Creates the directory and its parents; false when it does not exist afterwards.
bool makeDirectories(const std::string& dir)
{
    for (size_t i = 0; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == '\\')
        {
            const std::string part = dir.substr(0, i);
            if (part.size() > 2)    // past "C:"
                CreateDirectoryA(part.c_str(), 0);
        }
    const DWORD attributes = GetFileAttributesA(dir.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool validShotName(const std::string& name)
{
    if (name.empty() || name[0] == '.')
        return false;
    for (size_t i = 0; i < name.size(); ++i)
        if (!isalnum(static_cast<unsigned char>(name[i])) && name[i] != '_' && name[i] != '-'
            && name[i] != '.')
            return false;
    return true;
}

void shotFinished(const std::string& result)
{
    g_shot.queued = false;
    g_shot.requested = false;
    g_shot.last = result;
    if (g_shot.hudShown && g_viewToolActive)
        runConsole("renderer.drawHud 0");
    g_shot.hudShown = false;
    if (io::mainConsole)
        io::mainConsole->output(result + "\n");
}

// view.shot [NAME]: <outputDir>\<map>\shots\<NAME>.png and .jpg. NAME defaults to the held view's
// name (or "camera") and the local time; an existing NAME is overwritten.
std::string viewShot(const std::string& requested)
{
    if (g_shot.queued || g_shot.requested)
        return "view.shot: a shot is still pending (" + g_shot.png + ")";
    if (!rendererTools())
        return "view.shot: the renderer does not take shots";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "view.shot: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    std::string name = requested;
    if (name.empty())
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay,
                      t.wHour, t.wMinute, t.wSecond);
        name = (g_state.view.empty() ? std::string("camera") : g_state.view) + stamp;
    }
    if (!validShotName(name))
        return "view.shot: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\shots";
    if (!makeDirectories(dir))
        return "view.shot: cannot create " + dir;
    g_shot.png = dir + "\\" + name + ".png";
    g_shot.jpg = dir + "\\" + name + ".jpg";
    g_shot.hud = g_shotHud != 0;
    g_shot.hudShown = false;
    if (g_shot.hud && g_viewToolActive)
    {
        const std::string value = trimEnd(g_state.savedDrawHud);
        runConsole("renderer.drawHud " + (value.empty() ? std::string("1") : value));
        g_shot.hudShown = true;
    }
    g_shot.frames = 0;
    g_shot.queued = true;
    g_viewToolShooting = true;
    return "view.shot: queued " + g_shot.png;
}

std::string viewLastShot()
{
    if (g_shot.queued || g_shot.requested)
        return "pending " + g_shot.png;
    return g_shot.last.empty() ? std::string("none") : g_shot.last;
}

// Once per frame while a shot is queued or requested, before the frame draws.
void shotStep()
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
    {
        shotFinished("view.shot: failed: the renderer does not take shots");
        g_viewToolShooting = false;
        return;
    }
    if (g_shot.queued)
    {
        if (++g_shot.frames < (g_shot.hud ? kShotHudFrames : 1))
            return;
        tools->toolsRequestShot(g_shot.png.c_str(), g_shot.jpg.c_str(), g_shotWidth, g_shot.hud);
        g_shot.queued = false;
        g_shot.requested = true;
        g_shot.frames = 0;
        return;
    }
    const char* message = "";
    const int state = tools->toolsShotState(message);
    if (state == 0)
    {
        if (++g_shot.frames < kShotTimeoutFrames)
            return;
        tools->toolsRequestShot(0, 0, 0, false);    // cancels it
        shotFinished("view.shot: failed: no frame reached the capture point");
    }
    else if (state > 0)
        shotFinished("view.shot: wrote " + g_shot.png + " (" + message + ")");
    else
        shotFinished("view.shot: failed: " + std::string(message ? message : ""));
    g_viewToolShooting = false;
}

// tools.survey [NAME] (render milestone 11, task 1): <outputDir>\<map>\survey\<NAME>.txt, the
// renderer's report on the loaded level and the current view's water area (IRendererTools.h).
// NAME defaults to the held view's name (or "camera"); an existing file is overwritten.
struct Survey
{
    bool        pending;
    int         frames;     // hooks since requested
    std::string path;
    std::string last;       // the last survey's result, for tools.lastSurvey
};

Survey g_survey = { false, 0, std::string(), std::string() };

const int kSurveyTimeoutFrames = 120;

void surveyFinished(const std::string& result)
{
    g_survey.pending = false;
    g_survey.last = result;
    g_viewToolSurveying = false;
    if (io::mainConsole)
        io::mainConsole->output(result + "\n");
}

std::string toolsSurvey(const std::string& requested)
{
    if (g_survey.pending)
        return "tools.survey: a survey is still pending (" + g_survey.path + ")";
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.survey: the renderer does not survey";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.survey: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.survey: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\survey";
    if (!makeDirectories(dir))
        return "tools.survey: cannot create " + dir;
    g_survey.path = dir + "\\" + name + ".txt";
    g_survey.pending = true;
    g_survey.frames = 0;
    tools->toolsRequestSurvey(g_survey.path.c_str());
    g_viewToolSurveying = true;
    return "tools.survey: queued " + g_survey.path;
}

std::string toolsLastSurvey()
{
    if (g_survey.pending)
        return "pending " + g_survey.path;
    return g_survey.last.empty() ? std::string("none") : g_survey.last;
}

// tools.atmosphere [NAME] (render milestone 11): <outputDir>\<map>\atmosphere\<NAME>.txt and
// <NAME>-<table>.png, written at once: the atmosphere's fit and its tables as drawn. NAME defaults
// to the active view's name, else "camera".
std::string toolsAtmosphere(const std::string& requested)
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.atmosphere: the renderer has no tools";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.atmosphere: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.atmosphere: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\atmosphere";
    if (!makeDirectories(dir))
        return "tools.atmosphere: cannot create " + dir;
    const char* message = "";
    const bool ok = tools->toolsWriteAtmosphere((dir + "\\" + name).c_str(), message);
    return std::string(ok ? "tools.atmosphere: wrote " : "tools.atmosphere: failed: ")
         + (message ? message : "");
}

// tools.clouds [NAME] (render milestone 12): <outputDir>\<map>\clouds\<NAME>.txt and
// <NAME>-<volume>.png, written at once: the sky clouds' live volumes. NAME as for tools.atmosphere.
std::string toolsClouds(const std::string& requested)
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.clouds: the renderer has no tools";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.clouds: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.clouds: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\clouds";
    if (!makeDirectories(dir))
        return "tools.clouds: cannot create " + dir;
    const char* message = "";
    const bool ok = tools->toolsWriteClouds((dir + "\\" + name).c_str(), message);
    return std::string(ok ? "tools.clouds: wrote " : "tools.clouds: failed: ") + (message ? message : "");
}

// tools.exposure [NAME] (render milestone 13): <outputDir>\<map>\exposure\<NAME>.txt, written at
// once: the auto-exposure's state, histogram and meter, and the bloom. NAME as for tools.atmosphere.
std::string toolsExposure(const std::string& requested)
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.exposure: the renderer has no tools";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.exposure: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.exposure: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\exposure";
    if (!makeDirectories(dir))
        return "tools.exposure: cannot create " + dir;
    const char* message = "";
    const bool ok = tools->toolsWriteExposure((dir + "\\" + name).c_str(), message);
    return std::string(ok ? "tools.exposure: wrote " : "tools.exposure: failed: ") + (message ? message : "");
}

// tools.sun [NAME] (render milestone 14): <outputDir>\<map>\sun\<NAME>.txt and its PNGs, at once:
// the sun's static layers as drawn. NAME as for tools.atmosphere.
std::string toolsSun(const std::string& requested)
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.sun: the renderer has no tools";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.sun: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.sun: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\sun";
    if (!makeDirectories(dir))
        return "tools.sun: cannot create " + dir;
    const char* message = "";
    const bool ok = tools->toolsWriteSun((dir + "\\" + name).c_str(), message);
    return std::string(ok ? "tools.sun: wrote " : "tools.sun: failed: ") + (message ? message : "");
}

// tools.water [NAME] (render milestone 23): <outputDir>\<map>\water\<NAME>.txt and its PNGs, at once:
// the open-water map and its fetch table. NAME as for tools.atmosphere.
std::string toolsWater(const std::string& requested)
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
        return "tools.water: the renderer has no tools";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "tools.water: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    const std::string name = requested.empty()
        ? (g_state.view.empty() ? std::string("camera") : g_state.view) : requested;
    if (!validShotName(name))
        return "tools.water: a name has only letters, digits, '_', '-' and '.'";
    const std::string dir = outputDirectory() + "\\" + map + "\\water";
    if (!makeDirectories(dir))
        return "tools.water: cannot create " + dir;
    const char* message = "";
    const bool ok = tools->toolsWriteWater((dir + "\\" + name).c_str(), message);
    return std::string(ok ? "tools.water: wrote " : "tools.water: failed: ") + (message ? message : "");
}

// Once per frame while a survey is pending.
void surveyStep()
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools)
    {
        surveyFinished("tools.survey: failed: the renderer does not survey");
        return;
    }
    const char* message = "";
    const int state = tools->toolsSurveyState(message);
    if (state == 0)
    {
        if (++g_survey.frames < kSurveyTimeoutFrames)
            return;
        tools->toolsRequestSurvey(0);    // cancels it
        surveyFinished("tools.survey: failed: no report after " +
                       std::to_string(kSurveyTimeoutFrames) + " frames");
    }
    else if (state > 0)
        surveyFinished("tools.survey: wrote " + g_survey.path);
    else
        surveyFinished("tools.survey: failed: " + std::string(message ? message : ""));
}

// bench.run PATH [runs] [label] (milestone 3): one warm-up pass of the path, then `runs` passes
// back to back, each frame tagged with its pass for the renderer's recorder (0 for the warm-up,
// which is not recorded). Vsync is turned off and BF2Engine's frame limiter skipped for the
// whole benchmark; both come back when it ends. After the last pass the camera goes back to the
// game and the tool waits kBenchDrainFrames frames, still uncapped, for the last GPU times, then
// writes <outputDir>\<map>\bench\<PATH>[-<label>]-<time>.csv and .txt.
struct Bench
{
    enum Phase { Idle, Warmup, Running, Draining };
    Phase       phase;
    int         runs;
    int         run;            // the pass being drawn, 1..runs
    int         drainFrames;
    std::string path;
    std::string label;
    std::string map;
    std::string base;           // the output files without their extension
    std::string savedVsync;
    std::string last;           // the last benchmark's result, for bench.last
};

Bench g_bench = { Bench::Idle, 0, 0, 0, std::string(), std::string(), std::string(), std::string(),
                  std::string(), std::string() };

const int kBenchDrainFrames = 12;   // more than the GPU timers' slots in flight (GpuTimers.cpp)
const int kBenchMaxRuns = 20;
// A frame is GPU bound when Present blocked for at least this share of it: the driver holds
// Present while the GPU is frames behind. GPU pass spans cannot decide it, since they include the
// GPU's idle time while the CPU is the bottleneck (their sum is about the frame time either way).
const float kBenchGpuBoundWait = 0.1f;

bool benchGpuBound(const dx9rend::ToolsBenchFrame& f)
{
    return f.presentWaitMs >= kBenchGpuBoundWait * f.frameMs;
}

void benchRestore()
{
    if (dx9rend::IRendererTools* const tools = rendererTools())
        tools->toolsBenchStop();
    g_viewToolUncapped = false;
    const std::string vsync = trimEnd(g_bench.savedVsync);
    if (!vsync.empty())
        runConsole("renderer.vsync " + vsync);
    g_bench.phase = Bench::Idle;
    g_viewToolBenching = false;
}

void benchFinished(const std::string& result)
{
    g_bench.last = result;
    if (io::mainConsole)
        io::mainConsole->output(result + "\n");
}

// Stops a benchmark before its files are written.
void benchAbort(const std::string& why)
{
    if (g_bench.phase == Bench::Idle)
        return;
    benchRestore();
    benchFinished("bench.run: stopped (" + why + "); no files written");
}

// pathStep at the end of a pass: true when the benchmark starts its next pass with this frame,
// false when the path ends (the last pass, or no benchmark).
bool benchPathEnded()
{
    dx9rend::IRendererTools* const tools = rendererTools();
    if (!tools || (g_bench.phase != Bench::Warmup && g_bench.phase != Bench::Running))
        return false;
    if (g_bench.phase == Bench::Warmup || g_bench.run < g_bench.runs)
    {
        g_bench.run = g_bench.phase == Bench::Warmup ? 1 : g_bench.run + 1;
        g_bench.phase = Bench::Running;
        tools->toolsBenchTag(g_bench.run);
        return true;
    }
    tools->toolsBenchTag(0);
    g_bench.phase = Bench::Draining;
    g_bench.drainFrames = 0;
    return false;
}

// The frames of a selection, summarised.
struct BenchStats
{
    unsigned int frames;
    unsigned int gpuFrames;     // with GPU times
    double avgMs, lowMs, worstMs;   // frame time: mean, mean of the slowest 1 %, worst
    double cpuMs, waitMs, gpuMs;    // means: CPU busy (frame - Present wait), Present wait, GPU
    double draws, primitives;
    double gpuBound;            // share of the frames that were GPU bound (benchGpuBound)
};

BenchStats benchStats(const std::vector<dx9rend::ToolsBenchFrame>& frames, int tag)
{
    BenchStats st = {};
    std::vector<float> times;
    unsigned int bound = 0;
    for (size_t i = 0; i < frames.size(); ++i)
    {
        const dx9rend::ToolsBenchFrame& f = frames[i];
        if (tag != 0 && f.tag != tag)
            continue;
        const float cpu = (std::max)(0.0f, f.frameMs - f.presentWaitMs);
        times.push_back(f.frameMs);
        st.avgMs += f.frameMs;
        st.cpuMs += cpu;
        st.waitMs += f.presentWaitMs;
        st.draws += f.draws;
        st.primitives += f.primitives;
        if (benchGpuBound(f))
            ++bound;
        if (f.gpuMs >= 0.0f)
        {
            ++st.gpuFrames;
            st.gpuMs += f.gpuMs;
        }
    }
    st.frames = static_cast<unsigned int>(times.size());
    if (!st.frames)
        return st;
    st.avgMs /= st.frames;
    st.cpuMs /= st.frames;
    st.waitMs /= st.frames;
    st.draws /= st.frames;
    st.primitives /= st.frames;
    if (st.gpuFrames)
        st.gpuMs /= st.gpuFrames;
    st.gpuBound = static_cast<double>(bound) / st.frames;
    std::sort(times.begin(), times.end());
    const size_t slow = (std::max<size_t>)(1, (times.size() + 99) / 100);
    for (size_t i = times.size() - slow; i < times.size(); ++i)
        st.lowMs += times[i];
    st.lowMs /= slow;
    st.worstMs = times.back();
    return st;
}

std::string benchFormat(const char* format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

std::string benchStatsRow(const char* name, const BenchStats& st)
{
    return benchFormat("%-6s %7u %8.2f %7.1f %8.2f %7.1f %8.2f %7.2f %7.2f %7.2f %7.0f %8.2f %5.0f %%\n",
                       name, st.frames, st.avgMs, st.avgMs > 0.0 ? 1000.0 / st.avgMs : 0.0, st.lowMs,
                       st.lowMs > 0.0 ? 1000.0 / st.lowMs : 0.0, st.worstMs, st.cpuMs, st.waitMs,
                       st.gpuMs, st.draws, st.primitives / 1.0e6, st.gpuBound * 100.0);
}

// The spread of a value between the runs: (largest - smallest) / mean, in per cent.
double benchSpread(const std::vector<double>& values)
{
    if (values.size() < 2)
        return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < values.size(); ++i)
        sum += values[i];
    const double mean = sum / values.size();
    const double range = *std::max_element(values.begin(), values.end())
                       - *std::min_element(values.begin(), values.end());
    return mean > 0.0 ? range / mean * 100.0 : 0.0;
}

// Writes the CSV and the summary; returns the console's one-line result.
std::string benchWrite()
{
    dx9rend::IRendererTools* const tools = rendererTools();
    std::vector<dx9rend::ToolsBenchFrame> frames(tools->toolsBenchFrameCount());
    for (unsigned int i = 0; i < frames.size(); ++i)
        tools->toolsBenchFrame(i, frames[i]);
    if (frames.empty())
        return "bench.run: failed: no frames were recorded";
    const unsigned int spans = tools->toolsBenchSpanCount();

    const std::string csvPath = g_bench.base + ".csv";
    std::FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (!csv)
        return "bench.run: failed: cannot write " + csvPath;
    std::fprintf(csv, "run,frame,time_s,frame_ms,cpu_ms,present_wait_ms,gpu_ms,gpu_bound,draws,primitives");
    for (unsigned int s = 0; s < spans; ++s)
        std::fprintf(csv, ",gpu_%s", tools->toolsBenchSpanName(s));
    std::fprintf(csv, "\n");
    int run = 0;
    unsigned int frame = 0;
    double time = 0.0;
    for (unsigned int i = 0; i < frames.size(); ++i)
    {
        const dx9rend::ToolsBenchFrame& f = frames[i];
        if (f.tag != run)
        {
            run = f.tag;
            frame = 0;
            time = 0.0;
        }
        time += f.frameMs / 1000.0;
        const float cpu = (std::max)(0.0f, f.frameMs - f.presentWaitMs);
        std::fprintf(csv, "%d,%u,%.4f,%.3f,%.3f,%.3f,", f.tag, frame++, time, f.frameMs, cpu,
                     f.presentWaitMs);
        if (f.gpuMs >= 0.0f)
            std::fprintf(csv, "%.3f", f.gpuMs);
        std::fprintf(csv, ",%d,%u,%u", benchGpuBound(f) ? 1 : 0, f.draws, f.primitives);
        for (unsigned int s = 0; s < spans; ++s)
        {
            const float ms = tools->toolsBenchSpanMs(i, s);
            if (ms >= 0.0f)
                std::fprintf(csv, ",%.3f", ms);
            else
                std::fprintf(csv, ",");
        }
        std::fprintf(csv, "\n");
    }
    std::fclose(csv);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string text = benchFormat("bench %s%s%s on %s, %04u-%02u-%02u %02u:%02u:%02u\n",
                                   g_bench.path.c_str(), g_bench.label.empty() ? "" : ", label ",
                                   g_bench.label.c_str(), g_bench.map.c_str(), t.wYear, t.wMonth,
                                   t.wDay, t.wHour, t.wMinute, t.wSecond);
    text += benchFormat("path %.2f s, %d runs after a warm-up, no vsync or frame cap\n\n",
                        g_play.keys.empty() ? 0.0f : g_play.keys.back().time, g_bench.runs);
    text += "           frames  avg ms     fps 1%low ms     fps worst ms  cpu ms wait ms  gpu ms   draws  Mprims  gpu bound\n";
    std::vector<double> averages, lows;
    for (int r = 1; r <= g_bench.runs; ++r)
    {
        const BenchStats st = benchStats(frames, r);
        averages.push_back(st.avgMs);
        lows.push_back(st.lowMs);
        text += benchStatsRow(benchFormat("run %d", r).c_str(), st);
    }
    const BenchStats all = benchStats(frames, 0);
    text += benchStatsRow("all", all);
    const double spreadAvg = benchSpread(averages);
    const double spreadLow = benchSpread(lows);
    text += benchFormat("\nspread between runs: %.2f %% average frame time, %.2f %% 1%% low\n",
                        spreadAvg, spreadLow);
    text += benchFormat("GPU times: %u of %u frames (the rest dropped)\n", all.gpuFrames, all.frames);
    text += "\nGPU per pass, all runs (ms, mean over the frames it ran in):\n";
    for (unsigned int s = 0; s < spans; ++s)
    {
        double sum = 0.0;
        unsigned int count = 0;
        for (unsigned int i = 0; i < frames.size(); ++i)
        {
            const float ms = tools->toolsBenchSpanMs(i, s);
            if (ms >= 0.0f)
            {
                sum += ms;
                ++count;
            }
        }
        text += benchFormat("  %-24s %8.3f  (%u frames)\n", tools->toolsBenchSpanName(s),
                            count ? sum / count : 0.0, count);
    }
    text += "\ncpu ms is the frame time less the time Present blocked (wait ms). A frame is GPU bound\n"
            "when Present blocked for 10 % of it or more. GPU times are spans between timestamps and\n"
            "include the GPU's idle time when the CPU is the bottleneck.\n";

    const std::string txtPath = g_bench.base + ".txt";
    std::FILE* txt = std::fopen(txtPath.c_str(), "w");
    if (!txt)
        return "bench.run: failed: cannot write " + txtPath;
    std::fputs(text.c_str(), txt);
    std::fclose(txt);

    return benchFormat("bench.run: wrote %s.csv and .txt: avg %.2f ms (%.1f fps), 1%% low %.2f ms, "
                       "spread %.2f %% / %.2f %%, GPU bound %.0f %%",
                       g_bench.base.c_str(), all.avgMs, all.avgMs > 0.0 ? 1000.0 / all.avgMs : 0.0,
                       all.lowMs, spreadAvg, spreadLow, all.gpuBound * 100.0);
}

// Once per frame while bench.run runs, before the path's camera is set.
void benchStep()
{
    if (g_bench.phase == Bench::Warmup || g_bench.phase == Bench::Running)
    {
        if (!g_play.playing)
            benchAbort("the path was stopped");
        return;
    }
    if (g_bench.phase != Bench::Draining || ++g_bench.drainFrames < kBenchDrainFrames)
        return;
    const std::string result = benchWrite();
    benchRestore();
    benchFinished(result);
}

std::string benchRun(const std::string& name, const std::string& runsText, const std::string& label)
{
    if (g_viewToolBenching)
        return "bench.run: a benchmark is already running; bench.stop ends it";
    if (!rendererTools())
        return "bench.run: the renderer does not record benchmarks";
    std::string map = game ? game->getCurrentMapName() : std::string();
    if (map.empty())
        return "bench.run: no map loaded";
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = static_cast<char>(tolower(static_cast<unsigned char>(map[i])));
    char* end = 0;
    const long runs = runsText.empty() ? 3 : std::strtol(runsText.c_str(), &end, 10);
    if ((!runsText.empty() && (!end || *end)) || runs < 1 || runs > kBenchMaxRuns)
        return benchFormat("bench.run: runs is a whole number from 1 to %d", kBenchMaxRuns);
    if (!label.empty() && !validShotName(label))
        return "bench.run: a label has only letters, digits, '_', '-' and '.'";
    if (!validShotName(name))
        return "bench.run: no path " + name;
    const std::string dir = outputDirectory() + "\\" + map + "\\bench";
    if (!makeDirectories(dir))
        return "bench.run: cannot create " + dir;

    const std::string played = pathPlay(name);
    if (!g_play.playing || g_play.name != name)
        return "bench.run: " + played.substr(played.find(':') + 2);

    SYSTEMTIME t;
    GetLocalTime(&t);
    g_bench.base = dir + "\\" + name + (label.empty() ? std::string() : "-" + label)
                 + benchFormat("-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
                               t.wMinute, t.wSecond);
    g_bench.path = name;
    g_bench.label = label;
    g_bench.map = map;
    g_bench.runs = static_cast<int>(runs);
    g_bench.run = 0;
    g_bench.savedVsync = runConsole("renderer.vsync");
    runConsole("renderer.vsync 0");
    g_viewToolUncapped = true;
    rendererTools()->toolsBenchStart();
    rendererTools()->toolsBenchTag(0);
    g_bench.phase = Bench::Warmup;
    g_viewToolBenching = true;
    return benchFormat("bench.run: %s, a warm-up and %ld runs of %.2f s; results in %s.csv",
                       name.c_str(), runs, g_play.keys.back().time, g_bench.base.c_str());
}

void benchStop()
{
    if (g_bench.phase == Bench::Idle)
        return;
    benchAbort("bench.stop");
    pathStop();
}

std::string benchLast()
{
    if (g_viewToolBenching)
        return g_bench.phase == Bench::Draining
            ? std::string("running: writing")
            : benchFormat("running: %s %d of %d", g_bench.phase == Bench::Warmup ? "warm-up, then run"
                                                                                  : "run",
                          g_bench.phase == Bench::Warmup ? 1 : g_bench.run, g_bench.runs);
    return g_bench.last.empty() ? std::string("none") : g_bench.last;
}

// The Views tab's read-only window on the tool (IRendererUi.h).
class UiViewTool : public dx9rend::IUiViewTool
{
public:
    UiViewTool() : m_haveCheck(false) { }

    virtual void uiViewsRefresh()
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (m_haveCheck && now - m_lastCheck < std::chrono::milliseconds(500))
            return;
        m_haveCheck = true;
        m_lastCheck = now;
        std::string error;
        m_path = refreshViews(error) ? g_views.path() : std::string();
    }
    virtual const char* uiViewsPath() const { return m_path.c_str(); }
    virtual bool uiViewsFileExists() const { return !m_path.empty() && g_views.exists(); }
    virtual int uiViewCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.views().size());
    }
    virtual bool uiViewAt(int index, dx9rend::UiView& out) const
    {
        if (index < 0 || index >= uiViewCount())
            return false;
        const ViewDef& v = g_views.views()[index];
        out.name = v.name.c_str();
        out.note = v.note.c_str();
        out.x = v.x;
        out.y = v.y;
        out.z = v.z;
        out.yaw = v.yaw;
        out.pitch = v.pitch;
        out.fov = v.fov;
        out.hasFov = v.hasFov;
        out.runCount = static_cast<int>(v.runs.size());
        return true;
    }
    virtual const char* uiViewRun(int view, int run, bool& undone) const
    {
        undone = false;
        if (view < 0 || view >= uiViewCount() || run < 0
            || run >= static_cast<int>(g_views.views()[view].runs.size()))
            return "";
        const std::string& line = g_views.views()[view].runs[run];
        undone = !variableSetBy(line).empty();
        return line.c_str();
    }
    virtual int uiSetCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.sets().size());
    }
    virtual const char* uiSetName(int set) const
    {
        return set >= 0 && set < uiSetCount() ? g_views.sets()[set].name.c_str() : "";
    }
    virtual int uiSetViewCount(int set) const
    {
        return set >= 0 && set < uiSetCount() ? static_cast<int>(g_views.sets()[set].views.size())
                                              : 0;
    }
    virtual const char* uiSetView(int set, int index) const
    {
        return index >= 0 && index < uiSetViewCount(set) ? g_views.sets()[set].views[index].c_str()
                                                         : "";
    }
    virtual int uiViewProblemCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.problems().size());
    }
    virtual const char* uiViewProblem(int index) const
    {
        return index >= 0 && index < uiViewProblemCount() ? g_views.problems()[index].c_str() : "";
    }
    virtual bool uiViewActive() const { return g_viewToolActive; }
    virtual bool uiViewFlying() const { return g_state.fly; }
    virtual const char* uiHeldView() const { return g_state.view.c_str(); }
    virtual int uiPathCount() const
    {
        return m_path.empty() ? 0 : static_cast<int>(g_views.paths().size());
    }
    virtual bool uiPathAt(int index, dx9rend::UiPath& out) const
    {
        if (index < 0 || index >= uiPathCount())
            return false;
        const ViewPath& p = g_views.paths()[index];
        out.name = p.name.c_str();
        out.note = p.note.c_str();
        out.keyCount = static_cast<int>(p.keys.size());
        out.length = p.keys.empty() ? 0.0f : p.keys.back().time;
        return true;
    }
    virtual const char* uiPathKey(int path, int key) const
    {
        if (path < 0 || path >= uiPathCount() || key < 0
            || key >= static_cast<int>(g_views.paths()[path].keys.size()))
            return "";
        m_keyLine = pathKeyLine(g_views.paths()[path].keys[key]);
        return m_keyLine.c_str();
    }
    virtual const char* uiPlayingPath() const
    {
        return g_play.playing ? g_play.name.c_str() : "";
    }
    virtual float uiPathTime() const
    {
        if (!g_play.playing || !g_play.started)
            return 0.0f;
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - g_play.start).count();
    }

private:
    std::string m_path;
    mutable std::string m_keyLine;
    bool m_haveCheck;
    std::chrono::steady_clock::time_point m_lastCheck;
};

UiViewTool g_uiViewTool;

inline bool keyDown(int virtualKey)
{
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

// One frame of free fly: keys and the mouse's offset from the window centre, over the wall-clock
// time since the last frame.
void flyStep()
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    float dt = 0.0f;
    if (g_state.haveLastStep)
        dt = std::chrono::duration<float>(now - g_state.lastStep).count();
    g_state.lastStep = now;
    g_state.haveLastStep = true;
    if (dt > 0.1f)
        dt = 0.1f;

    Camera& c = g_state.camera;
    c.fov -= g_state.wheel * kFovStep;
    g_state.wheel = 0;
    if (c.fov < kMinFov) c.fov = kMinFov;
    if (c.fov > kMaxFov) c.fov = kMaxFov;

    // The keys and the mouse belong to the debug menu or the console while either is shown.
    HWND window = uiWantsMouse() ? 0 : gameWindow();
    showCursor(!window);
    if (!window)
    {
        g_state.recentre = true;
        return;
    }

    RECT client;
    GetClientRect(window, &client);
    POINT centre = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
    ClientToScreen(window, &centre);
    POINT cursor;
    GetCursorPos(&cursor);
    SetCursorPos(centre.x, centre.y);
    if (g_state.recentre)
    {
        g_state.recentre = false;
        return;
    }

    c.yaw   -= (cursor.x - centre.x) * g_flyMouse;
    c.pitch -= (cursor.y - centre.y) * g_flyMouse;
    if (c.pitch > 89.0f)  c.pitch = 89.0f;
    if (c.pitch < -89.0f) c.pitch = -89.0f;
    while (c.yaw > 180.0f)   c.yaw -= 360.0f;
    while (c.yaw <= -180.0f) c.yaw += 360.0f;

    float speed = g_flySpeed;
    if (keyDown(VK_SHIFT))
        speed *= 5.0f;
    else if (keyDown(VK_CONTROL))
        speed *= 0.2f;
    const float step = speed * dt;

    const float forward = (keyDown('W') ? 1.0f : 0.0f) - (keyDown('S') ? 1.0f : 0.0f);
    const float strafe  = (keyDown('D') ? 1.0f : 0.0f) - (keyDown('A') ? 1.0f : 0.0f);
    const float up      = (keyDown('E') || keyDown(VK_SPACE) ? 1.0f : 0.0f)
                        - (keyDown('Q') || keyDown('C') ? 1.0f : 0.0f);

    Mat4 m;
    cameraToMatrix(c, m);
    c.x += (m.m[8] * forward + m.m[0] * strafe) * step;
    c.y += (m.m[9] * forward + m.m[1] * strafe + up) * step;
    c.z += (m.m[10] * forward + m.m[2] * strafe) * step;
}

} // namespace

void viewToolFrame(dx9rend::IRenderView* view)
{
    if (g_viewToolShooting)
        shotStep();
    if (g_viewToolBenching)
        benchStep();
    if (g_viewToolSurveying)
        surveyStep();
    if (!view || !g_viewToolActive)
        return;
    if (g_state.seedFromGame)
    {
        cameraFromView(view, g_state.camera);
        g_state.seedFromGame = false;
    }
    if (g_state.seedFovFromGame)
    {
        g_state.camera.fov = view->getFov() / kDegToRad;
        g_state.seedFovFromGame = false;
    }
    if (g_play.playing && !pathStep(view))
        return;
    if (g_state.fly)
        flyStep();
    Mat4 m;
    cameraToMatrix(g_state.camera, m);
    view->setFov(g_state.camera.fov * kDegToRad);
    view->setViewMatrix(m.m);
}

// During free fly the game's player input is cleared, so neither a soldier nor the spawn menu
// moves; the camera reads the devices itself (flyStep).
bool viewToolFilterPlayerInput(io::PlayerInput& input)
{
    if (!g_state.fly)
        return true;
    std::memset(input.inputs, 0, sizeof(input.inputs));
    return false;
}

void viewToolMouseWheel(int delta)
{
    if (g_state.fly && !uiWantsMouse())    // the menu scrolls with it
        g_state.wheel += delta / WHEEL_DELTA;
}

void viewToolKeyDown(unsigned int virtualKey)
{
    if (g_flyKey != 0 && virtualKey == static_cast<unsigned int>(g_flyKey))
        setFly(g_state.fly ? 0 : 1);
}

// view.* commands
BF2_CONSOLE_OBJECT("view", 1, 0)
BF2_CONSOLE_PROPERTY_FN(fly, int, true, getFly(), setFly, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(leave, viewLeave, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(go, viewGo, std::string, true, std::string, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(save, viewSave, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(delete, viewDelete, std::string, true, std::string,
                                kViewToolConsoleTag)
BF2_CONSOLE_METHOD_2_RET_ACTIVE(move, viewMove, std::string, true, std::string, std::string,
                                kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(note, viewNote, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(set, viewSet, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(list, viewList, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(reload, viewReload, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(shot, viewShot, std::string, true, std::string, std::string(),
                                 kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(lastShot, viewLastShot, std::string, true, kViewToolConsoleTag)

// path.* commands
BF2_CONSOLE_OBJECT("path", 1, 0)
BF2_CONSOLE_METHOD_1_2_RET_ACTIVE(key, pathKey, std::string, true, std::string, std::string,
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_1_RET_ACTIVE(play, pathPlay, std::string, true, std::string, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(stop, pathStop, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(list, pathList, std::string, true, kViewToolConsoleTag)

// bench.* commands
BF2_CONSOLE_OBJECT("bench", 1, 0)
BF2_CONSOLE_METHOD_1_3_RET_ACTIVE(run, benchRun, std::string, true, std::string, std::string,
                                  std::string, std::string(), std::string(), kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_STATIC_ACTIVE(stop, benchStop, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(last, benchLast, std::string, true, kViewToolConsoleTag)

// tools.* variables
BF2_CONSOLE_OBJECT("tools", 1, 0)
BF2_CONSOLE_PROPERTY_FN(flySpeed, float, true, g_flySpeed, g_flySpeed =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyMouse, float, true, g_flyMouse, g_flyMouse =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(flyKey, int, true, g_flyKey, g_flyKey =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(outputDir, std::string, true, g_outputDir, g_outputDir =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(shotHud, int, true, g_shotHud, g_shotHud =, kViewToolConsoleTag)
BF2_CONSOLE_PROPERTY_FN(shotWidth, int, true, g_shotWidth, g_shotWidth =, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(survey, toolsSurvey, std::string, true, std::string, std::string(),
                                  kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_RET_ACTIVE(lastSurvey, toolsLastSurvey, std::string, true, kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(atmosphere, toolsAtmosphere, std::string, true, std::string,
                                  std::string(), kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(clouds, toolsClouds, std::string, true, std::string,
                                  std::string(), kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(exposure, toolsExposure, std::string, true, std::string,
                                  std::string(), kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(sun, toolsSun, std::string, true, std::string,
                                  std::string(), kViewToolConsoleTag)
BF2_CONSOLE_METHOD_0_1_RET_ACTIVE(water, toolsWater, std::string, true, std::string,
                                  std::string(), kViewToolConsoleTag)

dx9rend::IUiViewTool* viewToolUi()
{
    return &g_uiViewTool;
}

std::list<ConsoleObject*>& viewToolConsoleObjects()
{
    return consoleBindingList(kViewToolConsoleTag);
}

} // namespace hfe
} // namespace dice

#endif // !DEDICATED

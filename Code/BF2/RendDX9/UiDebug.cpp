// [UI] The debug menu and the performance overlay (docs/eon64/ui.md, milestone 3). Not part of retail BF2.
//
// Menu (the menu key), in tabs:
//   Commands  the mod's command file as buttons, grouped by section, with fields for arguments.
//   Views     [TOOLS] the map's views, sets and paths (docs/eon64/tools.md#1-views), through view.*
//             and path.* lines.
//   Shadows   the sun shadow variables.   Renderer  every other renderer.* variable, filtered.
//   Settings  the overlay, the console height, the menu key, the files.
// Variable widgets are generated from the console registry (range from the registry, help from
// rendererVariableHelp) and set their variable through the registry, as the console does; the menu
// owns no setting. Reset restores the value a variable had when the menu first showed it.
//
// Overlay (renderer.uiOverlay), top right: frame rate and a frame-time graph (frame, CPU, GPU),
// GPU time per pass from the timestamp queries (issued only while the overlay is shown), draw
// calls, process memory, display mode, map, camera and sun. Nothing here runs while it is hidden.
//
// Files, in the mod's settings directory (<GSModDirectory>/settings/), never in this repository:
//   eon64-ui.con        console lines run when the game hands the UI its host; rewritten when a UI
//                       setting changes (overlay, console height, menu key).
//   eon64-commands.txt  "[Section]" lines group; "label = console line | help" makes a button;
//                       "{name}" or "{name=default}" in the line adds a field; '#' starts a
//                       comment. A button runs its line as if typed into the console (rcon too).
#ifdef BF2_IMAGE_RENDDX9

#ifndef NOMINMAX
#define NOMINMAX          // std::min and std::max below
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   // GetProcessMemoryInfo from kernel32 (K32GetProcessMemoryInfo)
#endif
#include <windows.h>
#include <psapi.h>

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "GpuTimers.h"
#include "UiLayer.h"
#include "UiPanels.h"
#include "../RendMisc/IRendererUi.h"
#include "../RendMisc/IRenderView.h"
#include "../Math/Mat4.h"
#include "../Core/ConsoleObject.h"
#include "../Core/ConsoleObjects.h"
#include "../IO/Console/Console.h"
#include "../Geom/XpLightManager.h"
#include "../Game/Objects/Visual/LightSource.h"

#include "../External/imgui/imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

namespace geom { extern LightManager* g_LightManager; }
extern IBase* mainConsole;

namespace dx9rend {

extern void* g_rendererBase;

namespace {

const char kSettingsFile[] = "eon64-ui.con";
const char kCommandsFile[] = "eon64-commands.txt";
const float kPi = 3.14159265f;

RendDX9Base* base()
{
    return static_cast<RendDX9Base*>(g_rendererBase);
}

io::Console* console()
{
    return reinterpret_cast<io::Console*>(mainConsole);
}

// ---------------------------------------------------------------------------------------------
// Settings and commands

IUiHost* g_host = 0;
bool g_overlay = false;
bool g_loadingSettings = false;

std::string trim(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string settingsPath(const char* file)
{
    std::string dir = g_host ? g_host->uiModDirectory() : "";
    if (dir.empty())
        return std::string();
    if (dir[dir.size() - 1] != '/' && dir[dir.size() - 1] != '\\')
        dir += '/';
    return dir + "settings/" + file;
}

bool readLines(const std::string& path, std::vector<std::string>& lines)
{
    std::FILE* in = path.empty() ? 0 : std::fopen(path.c_str(), "r");
    if (!in)
        return false;
    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), in))
        lines.push_back(trim(buffer));
    std::fclose(in);
    return true;
}

} // namespace

void uiSaveSettings()
{
    if (g_loadingSettings || !g_host)
        return;
    const std::string path = settingsPath(kSettingsFile);
    std::FILE* out = path.empty() ? 0 : std::fopen(path.c_str(), "w");
    if (!out)
        return;
    std::fprintf(out, "rem Written by the openbf2-eon64 debug menu.\n");
    std::fprintf(out, "renderer.uiOverlay %d\n", g_overlay ? 1 : 0);
    std::fprintf(out, "renderer.uiConsoleHeight %g\n", uiLayerGetConsoleHeight());
    std::fprintf(out, "renderer.uiMenuKey %d\n", uiLayerGetMenuKey());
    std::fclose(out);
}

namespace {

void loadSettings()
{
    std::vector<std::string> lines;
    if (!readLines(settingsPath(kSettingsFile), lines) || !console())
        return;
    g_loadingSettings = true;
    std::string output;   // Console::ignoredString_ lives in BF2.exe and is not exported
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].empty() || lines[i].compare(0, 3, "rem") == 0)
            continue;
        console()->executeLine(lines[i] + "\n", output, false, true, false, false, true);
    }
    g_loadingSettings = false;
}

} // namespace

// Runs a line as if typed into the console and entered: the console's own access rules, echo
// and history, and rcon lines go to the server.
void uiRunTyped(const std::string& text)
{
    io::Console* const c = console();
    if (!c)
        return;
    std::string line = text.substr(0, 127);   // the console's line buffer holds 128 with Enter
    c->setCurrentLine(line);
    c->updateAsciiKey(13);
}

namespace {

// ---------------------------------------------------------------------------------------------
// Frame timing

struct Sample
{
    double time;       // seconds
    float frameMs;     // Present to Present
    float cpuMs;       // the frame without the time Present blocked
    float gpuMs;       // the GPU's frame, from the timestamp queries (a few frames old)
};

const unsigned int kSamples = 2048;
const double kWindowSeconds = 5.0;
Sample g_samples[kSamples];
unsigned int g_sampleHead = 0;    // next write
unsigned int g_sampleCount = 0;
double g_ticksPerMs = 0.0;
LONGLONG g_frameStart = 0;
LONGLONG g_presentStart = 0;
double g_presentWaitMs = 0.0;

LONGLONG ticks()
{
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return now.QuadPart;
}

const Sample& sample(unsigned int age)   // 0 = newest
{
    return g_samples[(g_sampleHead + kSamples - 1 - age) % kSamples];
}

struct FrameStats
{
    float avgMs, lowMs, worstMs;     // mean, 99th percentile, worst over the window
    float frameMs, cpuMs, gpuMs;     // smoothed latest
    unsigned int frames;
};

FrameStats frameStats()
{
    FrameStats s = {};
    if (!g_sampleCount)
        return s;
    const double now = sample(0).time;
    std::vector<float> window;
    window.reserve(g_sampleCount);
    double sum = 0.0;
    for (unsigned int i = 0; i < g_sampleCount; ++i)
    {
        const Sample& x = sample(i);
        if (now - x.time > kWindowSeconds)
            break;
        window.push_back(x.frameMs);
        sum += x.frameMs;
        if (x.frameMs > s.worstMs)
            s.worstMs = x.frameMs;
    }
    s.frames = static_cast<unsigned int>(window.size());
    s.avgMs = static_cast<float>(sum / window.size());
    const size_t p99 = (window.size() * 99) / 100;
    std::nth_element(window.begin(), window.begin() + std::min(p99, window.size() - 1), window.end());
    s.lowMs = window[std::min(p99, window.size() - 1)];
    // The latest values, averaged over the last 30 frames so they can be read.
    const unsigned int recent = std::min(g_sampleCount, 30u);
    for (unsigned int i = 0; i < recent; ++i)
    {
        s.frameMs += sample(i).frameMs / recent;
        s.cpuMs += sample(i).cpuMs / recent;
        s.gpuMs += sample(i).gpuMs / recent;
    }
    return s;
}

// ---------------------------------------------------------------------------------------------
// Menu

float uiScale()
{
    return ImGui::GetFontSize() / 16.0f;
}

std::vector<ConsoleObject*> g_variables;   // renderer set/get bindings, sorted by name
size_t g_variablesSeen = 0;                // registry size they were gathered at
std::map<const ConsoleObject*, std::string> g_firstValues;   // as first shown: what Reset restores

bool lessByName(const ConsoleObject* a, const ConsoleObject* b)
{
    return _stricmp(a->getFunctionName(), b->getFunctionName()) < 0;
}

void gatherVariables()
{
    if (!mainConsoleObjects)
        return;
    const ConsoleObjects::ConsoleObjectList& list = mainConsoleObjects->getConsoleObjects();
    if (list.size() == g_variablesSeen)
        return;
    g_variablesSeen = list.size();
    g_variables.clear();
    for (ConsoleObjects::ConsoleObjectList::const_iterator it = list.begin(); it != list.end(); ++it)
    {
        ConsoleObject* const o = *it;
        if (o->isSetGet() && o->getObjectName() && _stricmp(o->getObjectName(), "renderer") == 0)
            g_variables.push_back(o);
    }
    std::sort(g_variables.begin(), g_variables.end(), lessByName);
}

bool isType(const std::string& type, const char* a, const char* b = 0, const char* c = 0)
{
    return type == a || (b && type == b) || (c && type == c);
}

void setVariable(ConsoleObject* o, const std::string& value)
{
    std::string arg(value);
    o->execute(&arg, 1);
}

bool containsNoCase(const char* text, const char* part)
{
    const size_t n = std::strlen(part);
    if (!n)
        return true;
    for (; *text; ++text)
        if (_strnicmp(text, part, n) == 0)
            return true;
    return false;
}

// "sunShadowCasterReach" shown as "Caster reach" under its prefix's tab.
std::string displayName(const char* name, const char* prefix)
{
    std::string out;
    const size_t skip = prefix ? std::strlen(prefix) : 0;
    for (const char* p = name + skip; *p; ++p)
    {
        const char ch = *p;
        if (out.empty())
            out += static_cast<char>(toupper(ch));
        else if (isupper(static_cast<unsigned char>(ch)) && !isupper(static_cast<unsigned char>(p[-1])))
        {
            out += ' ';
            out += static_cast<char>(tolower(ch));
        }
        else
            out += ch;
    }
    return out;
}

void variableTooltip(ConsoleObject* o, const std::string& type, const std::string& value)
{
    if (!ImGui::BeginItemTooltip())
        return;
    const char* const help = rendererVariableHelp(o->getFunctionName());
    if (help)
        ImGui::TextUnformatted(help);
    ImGui::TextDisabled("renderer.%s", o->getFunctionName());
    if (o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        ImGui::TextDisabled("%s, %s .. %s, now %s", type.c_str(), r.first.c_str(), r.second.c_str(),
                            value.c_str());
    }
    else
        ImGui::TextDisabled("%s, now %s", type.c_str(), value.c_str());
    ImGui::EndTooltip();
}

// One table row: name, widget, and Reset while the value differs from the one first shown.
void drawVariableRow(ConsoleObject* o, const char* prefix)
{
    const std::string type = o->getReturnType() ? o->getReturnType() : "";
    ImGui::PushID(o);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const std::string label = displayName(o->getFunctionName(), prefix);
    if (!o->isActive())
    {
        ImGui::TextDisabled("%s", label.c_str());
        variableTooltip(o, type, "-");
        ImGui::TableNextColumn();
        ImGui::TextDisabled("inactive");
        ImGui::TableNextColumn();
        ImGui::PopID();
        return;
    }
    const std::string value = o->execute(0, 0);
    if (g_firstValues.find(o) == g_firstValues.end())
        g_firstValues[o] = value;
    ImGui::TextUnformatted(label.c_str());
    variableTooltip(o, type, value);

    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (isType(type, "bool"))
    {
        bool v = value == "1" || value == "true";
        if (ImGui::Checkbox("##v", &v))
            setVariable(o, v ? "1" : "0");
    }
    else if (isType(type, "float", "double") && o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        float v = static_cast<float>(std::atof(value.c_str()));
        if (ImGui::SliderFloat("##v", &v, static_cast<float>(std::atof(r.first.c_str())),
                               static_cast<float>(std::atof(r.second.c_str())), "%.3g"))
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", v);
            setVariable(o, text);
        }
    }
    else if (isType(type, "int", "uint", "unsigned int") && o->hasRange())
    {
        const std::pair<std::string, std::string> r = o->getRangeAsString();
        int v = std::atoi(value.c_str());
        if (ImGui::SliderInt("##v", &v, std::atoi(r.first.c_str()), std::atoi(r.second.c_str())))
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%d", v);
            setVariable(o, text);
        }
    }
    else
    {
        // Anything else is edited as text and applied with Enter, as typed into the console.
        char text[256];
        std::snprintf(text, sizeof(text), "%s", value.c_str());
        if (ImGui::InputText("##v", text, sizeof(text), ImGuiInputTextFlags_EnterReturnsTrue))
            setVariable(o, text);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        variableTooltip(o, type, value);

    ImGui::TableNextColumn();
    const std::string& first = g_firstValues[o];
    if (value != first)
    {
        if (ImGui::SmallButton("Reset"))
            setVariable(o, first);
        ImGui::SetItemTooltip("Back to %s", first.c_str());
    }
    ImGui::PopID();
}

void drawVariableTable(const char* id, const char* prefix, const char* filter, bool prefixOnly)
{
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
        | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 3, flags))
        return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 190.0f * uiScale());
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Reset").x
                                                                           + ImGui::GetStyle().FramePadding.x * 2.0f);
    const size_t prefixLength = std::strlen(prefix);
    for (size_t i = 0; i < g_variables.size(); ++i)
    {
        ConsoleObject* const o = g_variables[i];
        const bool hasPrefix = prefixLength && _strnicmp(o->getFunctionName(), prefix, prefixLength) == 0;
        if (hasPrefix != prefixOnly || !containsNoCase(o->getFunctionName(), filter))
            continue;
        drawVariableRow(o, prefixOnly ? prefix : 0);
    }
    ImGui::EndTable();
}

// Commands ----------------------------------------------------------------------------------

// The arguments of a command line: "{name}" or "{name=default}".
struct Argument
{
    std::string name;
    char value[96];
};

struct Command
{
    std::string section;
    std::string label;
    std::string line;
    std::string help;
    std::vector<Argument> args;
};
std::vector<Command> g_commands;
char g_commandFilter[64] = "";

void parseArguments(Command& c)
{
    size_t at = 0;
    while ((at = c.line.find('{', at)) != std::string::npos)
    {
        const size_t end = c.line.find('}', at);
        if (end == std::string::npos)
            break;
        const std::string spec = c.line.substr(at + 1, end - at - 1);
        const size_t eq = spec.find('=');
        Argument a;
        a.name = eq == std::string::npos ? spec : spec.substr(0, eq);
        std::snprintf(a.value, sizeof(a.value), "%s", eq == std::string::npos ? "" : spec.substr(eq + 1).c_str());
        c.args.push_back(a);
        at = end + 1;
    }
}

// The line with each argument's field in place of its placeholder.
std::string expand(const Command& c)
{
    std::string out;
    size_t at = 0;
    size_t arg = 0;
    while (at < c.line.size())
    {
        const size_t open = c.line.find('{', at);
        const size_t close = open == std::string::npos ? open : c.line.find('}', open);
        if (close == std::string::npos)
        {
            out += c.line.substr(at);
            break;
        }
        out += c.line.substr(at, open - at);
        if (arg < c.args.size())
            out += c.args[arg++].value;
        at = close + 1;
    }
    // Arguments left empty leave no trailing spaces.
    while (!out.empty() && out[out.size() - 1] == ' ')
        out.erase(out.size() - 1);
    return out;
}

void loadCommands()
{
    g_commands.clear();
    std::vector<std::string> lines;
    readLines(settingsPath(kCommandsFile), lines);
    std::string section = "Commands";
    for (size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == '#')
            continue;
        if (l[0] == '[' && l[l.size() - 1] == ']')
        {
            section = trim(l.substr(1, l.size() - 2));
            continue;
        }
        const size_t eq = l.find('=');
        if (eq == std::string::npos)
            continue;
        Command c;
        c.section = section;
        c.label = trim(l.substr(0, eq));
        std::string rest = l.substr(eq + 1);
        const size_t bar = rest.find(" | ");
        if (bar != std::string::npos)
        {
            c.help = trim(rest.substr(bar + 3));
            rest = rest.substr(0, bar);
        }
        c.line = trim(rest);
        parseArguments(c);
        if (!c.label.empty() && !c.line.empty())
            g_commands.push_back(c);
    }
    std::fprintf(stderr, "[UI] %u menu commands from %s\n", static_cast<unsigned int>(g_commands.size()),
                 settingsPath(kCommandsFile).c_str());
}

bool commandMatches(const Command& c)
{
    return containsNoCase(c.label.c_str(), g_commandFilter) || containsNoCase(c.line.c_str(), g_commandFilter)
        || containsNoCase(c.help.c_str(), g_commandFilter);
}

void commandTooltip(const Command& c)
{
    if (!ImGui::BeginItemTooltip())
        return;
    if (!c.help.empty())
        ImGui::TextUnformatted(c.help.c_str());
    ImGui::TextDisabled("%s", expand(c).c_str());
    ImGui::EndTooltip();
}

// A section: argument-free commands as a grid of equal buttons, then one row per command with
// arguments (its button, then a field per argument).
void drawSection(size_t first, size_t end)
{
    const float avail = ImGui::GetContentRegionAvail().x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float buttonWidth = 150.0f * uiScale();
    const int columns = std::max(1, static_cast<int>((avail + spacing) / (buttonWidth + spacing)));
    const float width = (avail - spacing * (columns - 1)) / columns;
    int column = 0;
    for (size_t i = first; i < end; ++i)
    {
        Command& c = g_commands[i];
        if (!c.args.empty() || !commandMatches(c))
            continue;
        ImGui::PushID(static_cast<int>(i));
        if (column)
            ImGui::SameLine();
        if (ImGui::Button(c.label.c_str(), ImVec2(width, 0.0f)))
            uiRunTyped(expand(c));
        commandTooltip(c);
        column = (column + 1) % columns;
        ImGui::PopID();
    }
    for (size_t i = first; i < end; ++i)
    {
        Command& c = g_commands[i];
        if (c.args.empty() || !commandMatches(c))
            continue;
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Button(c.label.c_str(), ImVec2(width, 0.0f)))
            uiRunTyped(expand(c));
        commandTooltip(c);
        const float fieldWidth = std::max(60.0f * uiScale(),
            (ImGui::GetContentRegionAvail().x - width - spacing * c.args.size()) / c.args.size());
        for (size_t a = 0; a < c.args.size(); ++a)
        {
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(a));
            ImGui::SetNextItemWidth(std::min(fieldWidth, 160.0f * uiScale()));
            if (ImGui::InputTextWithHint("##arg", c.args[a].name.c_str(), c.args[a].value,
                                         sizeof(c.args[a].value), ImGuiInputTextFlags_EnterReturnsTrue))
                uiRunTyped(expand(c));
            ImGui::SetItemTooltip("%s (Enter runs the command)", c.args[a].name.c_str());
            ImGui::PopID();
        }
        ImGui::PopID();
    }
}

void drawCommandsTab()
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Reload").x
                            - ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##cmdfilter", "filter commands", g_commandFilter, sizeof(g_commandFilter));
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        loadCommands();
    ImGui::SetItemTooltip("Reads %s again", settingsPath(kCommandsFile).c_str());

    if (g_commands.empty())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("No commands. One \"label = console line | help\" per line, \"[Section]\" "
                           "to group, \"{argument}\" or \"{argument=default}\" for a field, in");
        ImGui::TextDisabled("%s", settingsPath(kCommandsFile).c_str());
        return;
    }
    if (!ImGui::BeginChild("##commands", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }
    size_t first = 0;
    while (first < g_commands.size())
    {
        size_t end = first;
        bool any = false;
        while (end < g_commands.size() && g_commands[end].section == g_commands[first].section)
            any = commandMatches(g_commands[end++]) || any;
        if (any)
        {
            ImGui::SeparatorText(g_commands[first].section.c_str());
            drawSection(first, end);
        }
        first = end;
    }
    ImGui::EndChild();
}

// Tabs --------------------------------------------------------------------------------------

void drawShadowsTab()
{
    if (ImGui::BeginChild("##shadows", ImVec2(0.0f, 0.0f)))
        drawVariableTable("##sunvars", "sunShadow", "", true);
    ImGui::EndChild();
}

// [UI] Render milestone 11's atmosphere: renderer.atmosphere, the fit and its tables as drawn.
// Linear values shown as they are (no exposure), opaque: the multiple-scattering table is dark.
void atmosphereRgb(const char* label, const float* v)
{
    ImGui::Text("%-18s %.4f %.4f %.4f", label, v[0], v[1], v[2]);
}

// Tables are drawn opaque (the atlas's alpha is its transmittance, not coverage) and read as data:
// the backend leaves sRGB decode as the scene left stage 0.
void atmosphereOpaque(const ImDrawList*, const ImDrawCmd*)
{
    base()->device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    base()->device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
}

void atmosphereTable(const char* label, IDirect3DTexture9* texture, float width, float height)
{
    ImGui::TextDisabled("%s", label);
    if (!texture)
    {
        ImGui::TextDisabled("  not drawn");
        return;
    }
    const float scale = std::min(1.0f, ImGui::GetContentRegionAvail().x / width);
    ImDrawList* const list = ImGui::GetWindowDrawList();
    list->AddCallback(atmosphereOpaque, 0);
    ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)),
                 ImVec2(width * scale, height * scale));
    list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, 0);
}

void drawAtmosphereTab()
{
    if (!ImGui::BeginChild("##atmosphere", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }
    drawVariableTable("##atmosvars", "", "atmosphere", false);   // full names: no prefix
    drawVariableTable("##skygain", "", "skyGainBlend", false);
    drawVariableTable("##haze", "", "haze", false);
    const AtmosphereInfo a = rendererAtmosphereInfo();
    if (!a.fitted)
        ImGui::TextDisabled("No fit yet: it runs once a level with a sky dome and fog has drawn.");
    else
    {
        ImGui::Text("fit %.0f ms, reference height %.1f m, fog end %.0f m", a.fitMs, a.refHeight,
                    a.fogEnd);
        atmosphereRgb("rayleigh tint", a.rayleigh);
        ImGui::Text("%-18s %.2f, applied %.2f", "sky gain", a.skyGain, a.skyGainApplied);
        atmosphereRgb("sun above", a.sunTop);
        atmosphereRgb(a.haveZenith ? "zenith target" : "zenith (unread)", a.zenithTarget);
        atmosphereRgb("zenith model", a.zenithModel);
        atmosphereRgb("horizon target", a.horizonTarget);
        atmosphereRgb("horizon model", a.horizonModel);   // before the gain
        atmosphereRgb("ground albedo", a.groundAlbedo);
        ImGui::Text("%-18s %.3e /m", "haze density", a.hazeDensity);
        ImGui::Text("atlas %s: vertex fetch %s, filtered %s", a.atlasFloat32 ? "A32B32G32R32F" : "A16B16G16R16F",
                    a.atlasVertexFetch ? "yes" : "no", a.atlasVertexFilter ? "yes" : "no");
        ImGui::Separator();
        atmosphereTable("transmittance (x: view angle, y: altitude)", a.transmittance, 512.0f, 128.0f);
        atmosphereTable("multiple scattering (x: sun angle, y: altitude)", a.multiScatter, 128.0f, 128.0f);
        atmosphereTable("sky view (x: azimuth from the sun, y: zenith to nadir)", a.skyView, 384.0f, 216.0f);
        atmosphereTable("aerial perspective (32 slices near to far; rgb in-scatter)", a.perspective,
                        1024.0f, 32.0f);
    }
    ImGui::EndChild();
}

void drawRendererTab()
{
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##varfilter", "filter renderer variables", filter, sizeof(filter));
    if (ImGui::BeginChild("##renderer", ImVec2(0.0f, 0.0f)))
        drawVariableTable("##vars", "sunShadow", filter, false);
    ImGui::EndChild();
}

// Views ---------------------------------------------------------------------------------------
// [TOOLS] The view tool's views (docs/eon64/tools.md#1-views), read through IUiViewTool. Every
// change is a view.* console line; its result shows under the buttons.

std::string g_viewSelected;        // view name
std::string g_viewResult;          // the last line's result
std::string g_viewNoteFor;         // the view the note field was filled from
std::string g_viewEditingSet;      // the set whose field is being typed in
std::string g_viewDeleteArmed;     // the view whose Delete was pressed once
char g_viewSaveName[64] = "";
char g_viewSaveNote[128] = "";
char g_viewNote[128] = "";
char g_viewSetText[256] = "";
char g_viewNewSet[64] = "";
std::string g_pathSelected;        // path name
char g_pathKeyName[64] = "";
char g_pathKeyTime[16] = "";

// A note as one console argument: quoted, without quotes of its own.
std::string quoted(const char* text)
{
    std::string out;
    for (const char* c = text; *c; ++c)
        if (*c != '"')
            out += *c;
    return "\"" + trim(out) + "\"";
}

void runViewLine(const std::string& line)
{
    io::Console* const c = console();
    if (!c)
        return;
    std::string out;
    c->executeLine(line, out, false, true, false, false, false);
    g_viewResult = trim(out);
    if (g_viewResult.empty())
        g_viewResult = line;
}

std::string setLine(dx9rend::IUiViewTool* t, int set, const std::string& add, const std::string& drop)
{
    std::string views;
    for (int v = 0; v < t->uiSetViewCount(set); ++v)
    {
        const std::string name = t->uiSetView(set, v);
        if (name != drop)
            views += (views.empty() ? "" : " ") + name;
    }
    if (!add.empty())
        views += (views.empty() ? "" : " ") + add;
    return std::string("view.set ") + t->uiSetName(set) + " \"" + views + "\"";
}

void drawViewDetails(dx9rend::IUiViewTool* t, int index, const dx9rend::UiView& v, bool active)
{
    ImGui::SeparatorText(v.name);
    const bool held = std::strcmp(t->uiHeldView(), v.name) == 0;
    if (ImGui::Button("Go"))
        runViewLine(std::string("view.go ") + v.name);
    ImGui::SetItemTooltip("view.go %s (or double-click the row)", v.name);
    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Update"))
        runViewLine(std::string("view.save ") + v.name);
    ImGui::SetItemTooltip(active ? "view.save %s: the camera the tool holds now replaces the view's"
                                 : "Needs a camera held by the tool: free fly (F8) or a view", v.name);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(index == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up))
        runViewLine(std::string("view.move ") + v.name + " up");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("view.move %s up", v.name);
    ImGui::SameLine();
    ImGui::BeginDisabled(index + 1 >= t->uiViewCount());
    if (ImGui::ArrowButton("##down", ImGuiDir_Down))
        runViewLine(std::string("view.move ") + v.name + " down");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("view.move %s down", v.name);
    ImGui::SameLine();
    if (g_viewDeleteArmed == v.name)
    {
        if (ImGui::Button("Confirm delete"))
        {
            runViewLine(std::string("view.delete ") + v.name);
            g_viewDeleteArmed.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            g_viewDeleteArmed.clear();
    }
    else if (ImGui::Button("Delete"))
    {
        g_viewDeleteArmed = v.name;
    }
    ImGui::SetItemTooltip("view.delete %s: also drops it from every set", v.name);

    char fov[32] = "game's";
    if (v.hasFov)
        std::snprintf(fov, sizeof(fov), "%.1f", v.fov);
    ImGui::TextDisabled("pos %.2f, %.2f, %.2f   yaw %.2f   pitch %.2f   fov %s%s", v.x, v.y, v.z, v.yaw,
                        v.pitch, fov, held ? "   (held)" : "");

    if (g_viewNoteFor != v.name)
    {
        g_viewNoteFor = v.name;
        std::snprintf(g_viewNote, sizeof(g_viewNote), "%s", v.note);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##note", "note (Enter saves)", g_viewNote, sizeof(g_viewNote),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
    {
        const std::string note = trim(g_viewNote);
        runViewLine(std::string("view.note ") + v.name + (note.empty() ? "" : " " + quoted(g_viewNote)));
        g_viewNoteFor.clear();
    }
    ImGui::SetItemTooltip("view.note %s \"text\"; empty removes the note", v.name);

    for (int r = 0; r < v.runCount; ++r)
    {
        bool undone = false;
        const char* const line = t->uiViewRun(index, r, undone);
        ImGui::BulletText("run %s", line);
        if (!undone)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(command, not undone)");
        }
    }
    if (v.runCount == 0)
        ImGui::TextDisabled("No run lines; add them by hand under the view's line in the file.");
}

void drawViewSets(dx9rend::IUiViewTool* t)
{
    ImGui::SeparatorText("Sets");
    const bool selected = !g_viewSelected.empty();
    for (int s = 0; s < t->uiSetCount(); ++s)
    {
        const std::string name = t->uiSetName(s);
        ImGui::PushID(s);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(name.c_str());
        ImGui::SameLine(110.0f * uiScale());
        // The field being typed in keeps its own text; the others show the file's.
        char shown[sizeof(g_viewSetText)];
        std::string views;
        for (int v = 0; v < t->uiSetViewCount(s); ++v)
            views += (v ? " " : "") + std::string(t->uiSetView(s, v));
        std::snprintf(shown, sizeof(shown), "%s", views.c_str());
        const bool editing = g_viewEditingSet == name;
        char* const text = editing ? g_viewSetText : shown;
        const float buttons = ImGui::CalcTextSize("+ selected- selectedRemove").x
            + ImGui::GetStyle().FramePadding.x * 6.0f + ImGui::GetStyle().ItemSpacing.x * 3.0f;
        ImGui::SetNextItemWidth(std::max(80.0f * uiScale(), ImGui::GetContentRegionAvail().x - buttons));
        if (ImGui::InputText("##views", text, sizeof(g_viewSetText), ImGuiInputTextFlags_EnterReturnsTrue))
            runViewLine("view.set " + name + " \"" + trim(text) + "\"");
        if (ImGui::IsItemActivated())
        {
            g_viewEditingSet = name;
            std::snprintf(g_viewSetText, sizeof(g_viewSetText), "%s", text);
        }
        if (ImGui::IsItemDeactivated() && g_viewEditingSet == name)
            g_viewEditingSet.clear();
        ImGui::SetItemTooltip("The views in order, separated by spaces; Enter saves (view.set %s \"...\")",
                              name.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!selected);
        if (ImGui::Button("+ selected"))
            runViewLine(setLine(t, s, g_viewSelected, std::string()));
        ImGui::SameLine();
        if (ImGui::Button("- selected"))
            runViewLine(setLine(t, s, std::string(), g_viewSelected));
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove"))
            runViewLine("view.set " + name);
        ImGui::SetItemTooltip("view.set %s: removes the set, not its views", name.c_str());
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(160.0f * uiScale());
    ImGui::InputTextWithHint("##newset", "new set name", g_viewNewSet, sizeof(g_viewNewSet));
    ImGui::SameLine();
    ImGui::BeginDisabled(!selected || trim(g_viewNewSet).empty());
    if (ImGui::Button("Add set with the selected view"))
        runViewLine("view.set " + trim(g_viewNewSet) + " " + g_viewSelected);
    ImGui::EndDisabled();
}

// Paths (tools milestone 3): the list, Play and Stop, a key at the held camera, the selected
// path's keys (edited by hand in the file).
void drawViewPaths(dx9rend::IUiViewTool* t, bool active)
{
    ImGui::SeparatorText("Paths");
    const std::string playing = t->uiPlayingPath();
    dx9rend::UiPath p;
    int selected = -1;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
    if (t->uiPathCount() == 0)
        ImGui::TextDisabled("No paths yet: hold a camera (F8 or a view), then Add key.");
    else if (ImGui::BeginTable("##pathlist", 3, flags))
    {
        ImGui::TableSetupColumn("path", ImGuiTableColumnFlags_WidthFixed, 160.0f * uiScale());
        ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed, 110.0f * uiScale());
        ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < t->uiPathCount(); ++i)
        {
            if (!t->uiPathAt(i, p))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const std::string label = std::string(playing == p.name ? "> " : "  ") + p.name;
            if (ImGui::Selectable(label.c_str(), g_pathSelected == p.name,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                g_pathSelected = p.name;
                std::snprintf(g_pathKeyName, sizeof(g_pathKeyName), "%s", p.name);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    runViewLine(std::string("path.play ") + p.name);
            }
            ImGui::TableNextColumn();
            ImGui::Text("%d keys, %.1f s", p.keyCount, p.length);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.note);
            ImGui::PopID();
            if (g_pathSelected == p.name)
                selected = i;
        }
        ImGui::EndTable();
    }
    if (selected < 0)
        g_pathSelected.clear();

    ImGui::BeginDisabled(selected < 0);
    if (ImGui::Button("Play"))
        runViewLine("path.play " + g_pathSelected);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("path.play NAME (or double-click): real time, then the camera goes back");
    ImGui::SameLine();
    ImGui::BeginDisabled(playing.empty());
    if (ImGui::Button("Stop"))
        runViewLine("path.stop");
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("path.stop: hands the camera back");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f * uiScale());
    ImGui::InputTextWithHint("##keypath", "path name", g_pathKeyName, sizeof(g_pathKeyName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f * uiScale());
    ImGui::InputTextWithHint("##keytime", "time (s)", g_pathKeyTime, sizeof(g_pathKeyTime),
                             ImGuiInputTextFlags_CharsDecimal);
    ImGui::SetItemTooltip("Seconds from the path's start; empty: 0 for a new path, else the last "
                          "key + 5 s. A key at the same time is replaced.");
    ImGui::SameLine();
    const std::string pathName = trim(g_pathKeyName);
    const bool badName = pathName.find_first_of(" \t|#=\"") != std::string::npos;
    ImGui::BeginDisabled(!active || pathName.empty() || badName);
    if (ImGui::Button("Add key"))
    {
        const std::string time = trim(g_pathKeyTime);
        runViewLine("path.key " + pathName + (time.empty() ? std::string() : " " + time));
        g_pathSelected = pathName;
        g_pathKeyTime[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(active ? "path.key NAME [TIME]: the camera the tool holds"
                                 : "Needs a camera held by the tool: free fly (F8) or a view");
    if (badName)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "A path name has no spaces, '|', '#', '=' or quotes.");

    if (selected >= 0 && t->uiPathAt(selected, p))
        for (int k = 0; k < p.keyCount; ++k)
            ImGui::TextDisabled("  %s", t->uiPathKey(selected, k));
}

void drawViewsTab()
{
    dx9rend::IUiViewTool* const t = g_host ? g_host->uiViewTool() : 0;
    if (!t)
    {
        ImGui::TextDisabled("The view tool is not available.");
        return;
    }
    t->uiViewsRefresh();
    const char* const path = t->uiViewsPath();
    if (!*path)
    {
        ImGui::TextDisabled("No map loaded: views belong to a map.");
        return;
    }
    const bool active = t->uiViewActive();

    // The camera: who holds it, free fly, leave, reload.
    ImGui::AlignTextToFramePadding();
    if (!active)
        ImGui::TextUnformatted("Game camera");
    else if (*t->uiPlayingPath())
        ImGui::Text("Path %s, %.1f s", t->uiPlayingPath(), t->uiPathTime());
    else if (*t->uiHeldView())
        ImGui::Text("View %s%s", t->uiHeldView(), t->uiViewFlying() ? ", free fly" : "");
    else
        ImGui::TextUnformatted("Free fly");
    ImGui::SameLine();
    bool fly = t->uiViewFlying();
    if (ImGui::Checkbox("Free fly", &fly))
        runViewLine(fly ? "view.fly 1" : "view.fly 0");
    ImGui::SetItemTooltip("view.fly (tools.flyKey, F8): flies once the menu closes");
    ImGui::SameLine();
    ImGui::BeginDisabled(!active);
    if (ImGui::Button("Leave"))
        runViewLine("view.leave");
    ImGui::SetItemTooltip("view.leave: hands the camera back and restores the view's variables");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload"))
        runViewLine("view.reload");
    ImGui::SetItemTooltip("view.reload: rereads %s", path);
    ImGui::TextDisabled("%s%s", path, t->uiViewsFileExists() ? "" : " (no file yet)");

    // Save the camera the tool holds as a new view, or over one of the same name.
    ImGui::SetNextItemWidth(140.0f * uiScale());
    ImGui::InputTextWithHint("##savename", "name", g_viewSaveName, sizeof(g_viewSaveName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f * uiScale(), ImGui::GetContentRegionAvail().x
        - ImGui::CalcTextSize("Save current view").x - ImGui::GetStyle().FramePadding.x * 2.0f
        - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##savenote", "note (optional)", g_viewSaveNote, sizeof(g_viewSaveNote));
    ImGui::SameLine();
    // The name is one console argument: a space would make the rest the note and overwrite
    // another view, so such a name is refused here with the reason.
    const std::string saveName = trim(g_viewSaveName);
    const bool badName = saveName.find_first_of(" \t|#=\"") != std::string::npos;
    bool exists = false;
    dx9rend::UiView existing;
    for (int i = 0; i < t->uiViewCount() && !exists; ++i)
        exists = t->uiViewAt(i, existing) && saveName == existing.name;
    const std::string saveLabel = exists ? "Update " + saveName + "##save" : "Save current view##save";
    ImGui::BeginDisabled(!active || saveName.empty() || badName);
    if (ImGui::Button(saveLabel.c_str()))
    {
        runViewLine("view.save " + saveName
                    + (trim(g_viewSaveNote).empty() ? std::string() : " " + quoted(g_viewSaveNote)));
        g_viewSelected = saveName;
        g_viewSaveName[0] = '\0';
        g_viewSaveNote[0] = '\0';
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(active ? "view.save NAME [note]: the camera the tool holds"
                                 : "Needs a camera held by the tool: free fly (F8) or a view");
    if (badName)
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                           "A view name has no spaces, '|', '#', '=' or quotes (high_2, not high 2).");
    if (!g_viewResult.empty())
        ImGui::TextWrapped("%s", g_viewResult.c_str());

    if (!ImGui::BeginChild("##views", ImVec2(0.0f, 0.0f)))
    {
        ImGui::EndChild();
        return;
    }
    ImGui::SeparatorText("Views");
    int selected = -1;
    dx9rend::UiView v;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
    if (t->uiViewCount() == 0)
        ImGui::TextDisabled("No views yet: fly with F8, close the menu, then Save current view.");
    else if (ImGui::BeginTable("##viewlist", 2, flags))
    {
        ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthFixed, 160.0f * uiScale());
        ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < t->uiViewCount(); ++i)
        {
            if (!t->uiViewAt(i, v))
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const bool held = std::strcmp(t->uiHeldView(), v.name) == 0;
            const std::string label = std::string(held ? "* " : "  ") + v.name;
            if (ImGui::Selectable(label.c_str(), g_viewSelected == v.name,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                g_viewSelected = v.name;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    runViewLine(std::string("view.go ") + v.name);
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.note);
            ImGui::PopID();
            if (g_viewSelected == v.name)
                selected = i;
        }
        ImGui::EndTable();
    }
    if (selected < 0)
        g_viewSelected.clear();
    if (selected >= 0 && t->uiViewAt(selected, v))
    {
        ImGui::PushID("selected");
        drawViewDetails(t, selected, v, active);
        ImGui::PopID();
    }
    drawViewSets(t);
    drawViewPaths(t, active);
    if (t->uiViewProblemCount() > 0)
    {
        ImGui::SeparatorText("Lines not understood");
        for (int i = 0; i < t->uiViewProblemCount(); ++i)
            ImGui::TextDisabled("%s", t->uiViewProblem(i));
    }
    ImGui::EndChild();
}

std::string keyName(int vk)
{
    char name[64] = "";
    const UINT scan = ::MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT)
        lParam |= 1 << 24;   // the extended keys, not their numeric-pad twins
    if (!::GetKeyNameTextA(lParam, name, sizeof(name)))
        std::snprintf(name, sizeof(name), "key %d", vk);
    return name;
}

void drawSettingsTab()
{
    bool overlay = g_overlay;
    if (ImGui::Checkbox("Performance overlay", &overlay))
        uiLayerSetOverlay(overlay);
    ImGui::SetItemTooltip("renderer.uiOverlay: top right; fps, frame-time graph, GPU passes, draws, memory");

    float height = uiLayerGetConsoleHeight();
    ImGui::SetNextItemWidth(220.0f * uiScale());
    if (ImGui::SliderFloat("Console height", &height, 0.2f, 0.9f, "%.2f"))
        uiLayerSetConsoleHeight(height);
    ImGui::SetItemTooltip("renderer.uiConsoleHeight: the console window's share of the screen height");

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Menu key: %s", keyName(uiLayerGetMenuKey()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(renderer.uiMenuKey %d)", uiLayerGetMenuKey());

    ImGui::SeparatorText("Files");
    ImGui::TextDisabled("Saved settings  %s", settingsPath(kSettingsFile).c_str());
    ImGui::TextDisabled("Commands        %s", settingsPath(kCommandsFile).c_str());
}

// ---------------------------------------------------------------------------------------------
// Overlay

void drawGraph(float width, float height, float worstMs)
{
    ImDrawList* const draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 end(origin.x + width, origin.y + height);
    draw->AddRectFilled(origin, end, IM_COL32(0, 0, 0, 110));

    // The scale: the smallest of these that holds the worst frame.
    static const float kScales[] = { 16.7f, 33.3f, 50.0f, 100.0f, 250.0f, 1000.0f };
    float topMs = kScales[0];
    for (unsigned int i = 0; i < sizeof(kScales) / sizeof(kScales[0]) && topMs < worstMs; ++i)
        topMs = kScales[i];
    const float yOf16 = end.y - height * (16.7f / topMs);
    draw->AddLine(ImVec2(origin.x, yOf16), ImVec2(end.x, yOf16), IM_COL32(255, 255, 255, 50));

    if (g_sampleCount < 2)
    {
        ImGui::Dummy(ImVec2(width, height));
        return;
    }
    const double now = sample(0).time;
    std::vector<ImVec2> frame, cpu, gpu;
    for (unsigned int i = 0; i < g_sampleCount; ++i)
    {
        const Sample& s = sample(i);
        const double age = now - s.time;
        if (age > kWindowSeconds)
            break;
        const float x = end.x - static_cast<float>(age / kWindowSeconds) * width;
        const auto y = [&](float ms) { return end.y - height * std::min(ms / topMs, 1.0f); };
        frame.push_back(ImVec2(x, y(s.frameMs)));
        cpu.push_back(ImVec2(x, y(s.cpuMs)));
        if (s.gpuMs > 0.0f)
            gpu.push_back(ImVec2(x, y(s.gpuMs)));
    }
    const float thickness = 1.0f * uiScale();
    draw->AddPolyline(cpu.data(), static_cast<int>(cpu.size()), IM_COL32(90, 200, 255, 200), 0, thickness);
    if (gpu.size() > 1)
        draw->AddPolyline(gpu.data(), static_cast<int>(gpu.size()), IM_COL32(255, 160, 60, 220), 0, thickness);
    draw->AddPolyline(frame.data(), static_cast<int>(frame.size()), IM_COL32(255, 255, 255, 230), 0, thickness);
    char label[32];
    std::snprintf(label, sizeof(label), "%.0f ms", topMs);
    draw->AddText(ImVec2(origin.x + 3.0f, origin.y + 1.0f), IM_COL32(255, 255, 255, 140), label);
    ImGui::Dummy(ImVec2(width, height));
}

void drawPasses()
{
    GpuSpanTime spans[48];
    const unsigned int n = gpuTimersSnapshot(spans, 48);
    if (!n)
    {
        ImGui::TextDisabled("GPU passes: waiting for timestamps");
        return;
    }
    if (!ImGui::BeginTable("passes", 2, ImGuiTableFlags_SizingFixedFit))
        return;
    for (unsigned int i = 0; i < n; ++i)
    {
        if (std::strcmp(spans[i].name, "(frame)") == 0)
            continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(spans[i].name);
        ImGui::TableNextColumn();
        ImGui::Text("%6.2f ms", spans[i].ms);
    }
    ImGui::EndTable();
}

void drawCounts()
{
    const unsigned int* const stats = reinterpret_cast<const unsigned int*>(base()->renderStats);
    // The last frame's row (renderStats words 54..94): indexed, plain and user-pointer draws.
    const unsigned int draws = stats[72] + stats[74] + stats[76];
    ImGui::Text("Draws %u  (%.2fM primitives)", draws, stats[71] / 1.0e6f);
    ImGui::Text("Sun draws %u / %u / %u / %u", sunShadowCascadeDraws(0), sunShadowCascadeDraws(1),
                sunShadowCascadeDraws(2), sunShadowCascadeDraws(3));

    static double lastMemory = -1.0;
    static PROCESS_MEMORY_COUNTERS_EX memory = {};
    const double now = ImGui::GetTime();
    if (now - lastMemory > 0.5 || lastMemory < 0.0)
    {
        lastMemory = now;
        ::GetProcessMemoryInfo(::GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    }
    ImGui::Text("Memory %.0f MB private, %.0f MB working set", memory.PrivateUsage / 1048576.0,
                memory.WorkingSetSize / 1048576.0);
}

void drawWhere()
{
    const D3DPRESENT_PARAMETERS& pp = base()->pp;
    const int samples = pp.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES ? static_cast<int>(pp.MultiSampleType) : 0;
    const char* const map = g_host ? g_host->uiMapName() : "";
    const char* const mode = g_host ? g_host->uiGameMode() : "";
    if (samples)
        ImGui::Text("%ux%u  MSAA %dx", pp.BackBufferWidth, pp.BackBufferHeight, samples);
    else
        ImGui::Text("%ux%u  no MSAA", pp.BackBufferWidth, pp.BackBufferHeight);
    if (map && *map)
        ImGui::Text("%s  %s", map, mode ? mode : "");

    IRenderView* const view = rendererGetRenderView();
    if (view && view->getViewMatrixPtr())
    {
        const float* const c = view->getViewMatrixPtr()->m;
        float heading = std::atan2(c[8], c[10]) * 180.0f / kPi;
        if (heading < 0.0f)
            heading += 360.0f;
        const float pitch = std::asin(std::max(-1.0f, std::min(1.0f, c[9]))) * 180.0f / kPi;
        ImGui::Text("Camera %.1f %.1f %.1f  heading %.0f  pitch %.0f", c[12], c[13], c[14], heading, pitch);
    }

    world::LightSource* const sky = geom::g_LightManager ? geom::g_LightManager->getSkyLight() : 0;
    if (sky)
    {
        const Vec3& d = sky->getDirection();   // the direction the light travels
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (length > 1e-6f)
        {
            const float elevation = std::asin(std::max(-1.0f, std::min(1.0f, -d.y / length))) * 180.0f / kPi;
            float azimuth = std::atan2(-d.x, -d.z) * 180.0f / kPi;
            if (azimuth < 0.0f)
                azimuth += 360.0f;
            ImGui::Text("Sun elevation %.0f  azimuth %.0f", elevation, azimuth);
        }
    }
}

} // namespace

void uiDebugSetHost(IUiHost* host)
{
    if (g_host || !host)
        return;
    g_host = host;
    loadSettings();
    loadCommands();
}

IUiHost* uiDebugHost()
{
    return g_host;
}

bool uiOverlayShown()
{
    return g_overlay;
}

bool uiLayerGetOverlay()
{
    return g_overlay;
}

void uiLayerSetOverlay(bool on)
{
    if (on == g_overlay)
        return;
    g_overlay = on;
    gpuTimersSetLive(on);
    g_sampleCount = 0;
    g_frameStart = 0;
    uiSaveSettings();
}

void uiDebugFrameStart()
{
    if (!g_overlay)
        return;
    if (g_ticksPerMs == 0.0)
    {
        LARGE_INTEGER frequency;
        ::QueryPerformanceFrequency(&frequency);
        g_ticksPerMs = static_cast<double>(frequency.QuadPart) / 1000.0;
    }
    const LONGLONG now = ticks();
    if (g_frameStart)
    {
        Sample& s = g_samples[g_sampleHead];
        s.time = static_cast<double>(now) / (g_ticksPerMs * 1000.0);
        s.frameMs = static_cast<float>((now - g_frameStart) / g_ticksPerMs);
        s.cpuMs = std::max(0.0f, s.frameMs - static_cast<float>(g_presentWaitMs));
        s.gpuMs = gpuTimersFrameMs();
        g_sampleHead = (g_sampleHead + 1) % kSamples;
        if (g_sampleCount < kSamples)
            ++g_sampleCount;
    }
    g_frameStart = now;
}

void uiDebugBeforePresent()
{
    if (g_overlay)
        g_presentStart = ticks();
}

void uiDebugPresented()
{
    if (g_overlay && g_presentStart)
        g_presentWaitMs = (ticks() - g_presentStart) / g_ticksPerMs;
}

void uiDebugDrawMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * uiScale(), viewport->WorkSize.y * 0.72f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * uiScale(), 300.0f * uiScale()),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("openbf2-eon64", 0, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return;
    }
    // Header: frame rate, map, and the overlay switch on the right.
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.0f fps", io.Framerate);
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f ms", io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    const char* const map = g_host ? g_host->uiMapName() : "";
    if (map && *map)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("  %s  %s", map, g_host->uiGameMode());
    }
    const char* const overlayLabel = "Overlay";
    const float checkWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x
        + ImGui::CalcTextSize(overlayLabel).x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - checkWidth));
    bool overlay = g_overlay;
    if (ImGui::Checkbox(overlayLabel, &overlay))
        uiLayerSetOverlay(overlay);

    gatherVariables();
    if (ImGui::BeginTabBar("##tabs"))
    {
        if (ImGui::BeginTabItem("Commands"))
        {
            drawCommandsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Views"))
        {
            drawViewsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Shadows"))
        {
            drawShadowsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Atmosphere"))
        {
            drawAtmosphereTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Renderer"))
        {
            drawRendererTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings"))
        {
            drawSettingsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void uiDebugDrawOverlay()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 8.0f * uiScale();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad, viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (!ImGui::Begin("##overlay", 0, flags))
    {
        ImGui::End();
        return;
    }
    const FrameStats s = frameStats();
    const auto fps = [](float ms) { return ms > 0.0f ? 1000.0f / ms : 0.0f; };
    ImGui::Text("%.0f fps  %.2f ms", fps(s.frameMs), s.frameMs);
    ImGui::Text("avg %.0f  1%% low %.0f  worst %.1f ms", fps(s.avgMs), fps(s.lowMs), s.worstMs);
    ImGui::TextColored(ImVec4(0.35f, 0.78f, 1.0f, 1.0f), "CPU %.2f ms", s.cpuMs);
    ImGui::SameLine();
    if (s.gpuMs > 0.0f)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.24f, 1.0f), "GPU %.2f ms", s.gpuMs);
        ImGui::SameLine();
        ImGui::TextDisabled(s.gpuMs > s.cpuMs ? "GPU-bound" : "CPU-bound");
    }
    else
        ImGui::TextDisabled("GPU -");
    drawGraph(300.0f * uiScale(), 70.0f * uiScale(), s.worstMs);
    ImGui::Separator();
    drawPasses();
    ImGui::Separator();
    drawCounts();
    ImGui::Separator();
    drawWhere();
    ImGui::End();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

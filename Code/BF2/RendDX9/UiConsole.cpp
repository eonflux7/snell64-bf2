// [UI] The console window (docs/lanes/ui.md, milestone 4). Not part of retail BF2.
//
// Replaces the retail drop-down console's drawing and typing over the same backend (BF2.exe's
// io::Console and the console object registry). Shown while the console is active (the game's
// console key still toggles it). The log is the console's own history; a line entered here runs as
// if typed into the retail console and entered (setCurrentLine, then Enter through
// updateAsciiKey), so access rules, echo, history, .con execution and rcon are the console's.
// Here: completion from the registry with types, ranges and help (Tab completes, then cycles),
// the window's own command history, a filter over the log, colours for entered lines, errors and
// warnings, copy and paste, and the window height (renderer.uiConsoleHeight).
#ifdef BF2_IMAGE_RENDDX9

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "UiLayer.h"
#include "UiPanels.h"
#include "../Core/ConsoleObject.h"
#include "../Core/ConsoleObjects.h"
#include "../IO/Console/Console.h"

#include "../External/imgui/imgui.h"
#include "../External/imgui/imgui_internal.h"   // BringWindowToDisplayFront for the completion list

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

extern IBase* mainConsole;

namespace dx9rend {

namespace {

io::Console* console()
{
    return reinterpret_cast<io::Console*>(mainConsole);
}

// The registry's names for completion: "object.function", with a one-line signature.
struct Name
{
    std::string name;
    std::string signature;
    const char* help;
};
std::vector<Name> g_names;
size_t g_namesSeen = 0;

bool lessName(const Name& a, const Name& b)
{
    return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
}

std::string signatureOf(ConsoleObject* o)
{
    std::string text;
    const char* const ret = o->getReturnType();
    if (o->isSetGet())
    {
        text = o->getArgType(0) ? o->getArgType(0) : (ret ? ret : "");
        if (o->hasRange())
        {
            const std::pair<std::string, std::string> r = o->getRangeAsString();
            text += "  " + r.first + " .. " + r.second;
        }
        return text;
    }
    text = "(";
    for (int i = 0; i < o->getMaxNoArgs(); ++i)
    {
        if (i)
            text += ", ";
        if (i == o->getMinNoArgs() && i)
            text += "[";
        text += o->getArgType(i) ? o->getArgType(i) : "?";
    }
    if (o->getMaxNoArgs() > o->getMinNoArgs() && o->getMinNoArgs())
        text += "]";
    text += ")";
    if (ret && *ret && std::strcmp(ret, "void") != 0)
        text += std::string(" -> ") + ret;
    return text;
}

void gatherNames()
{
    if (!mainConsoleObjects)
        return;
    const ConsoleObjects::ConsoleObjectList& list = mainConsoleObjects->getConsoleObjects();
    if (list.size() == g_namesSeen)
        return;
    g_namesSeen = list.size();
    g_names.clear();
    for (ConsoleObjects::ConsoleObjectList::const_iterator it = list.begin(); it != list.end(); ++it)
    {
        ConsoleObject* const o = *it;
        const char* const object = o->getObjectName();
        const char* const function = o->getFunctionName();
        if (!function)
            continue;
        Name n;
        n.name = object && *object ? std::string(object) + "." + function : std::string(function);
        n.signature = signatureOf(o);
        n.help = object && _stricmp(object, "renderer") == 0 ? rendererVariableHelp(function) : 0;
        g_names.push_back(n);
    }
    std::sort(g_names.begin(), g_names.end(), lessName);
    // A name registered twice (one binding per class that shares it) is listed once.
    std::vector<Name>::iterator last = std::unique(g_names.begin(), g_names.end(),
        [](const Name& a, const Name& b) { return _stricmp(a.name.c_str(), b.name.c_str()) == 0; });
    g_names.erase(last, g_names.end());
}

// The window's state.
const int kLineSize = 128;   // the console's line buffer: 127 characters and Enter
char g_input[kLineSize] = "";
char g_filter[64] = "";
std::vector<std::string> g_history;   // oldest first
int g_historyPos = -1;                // -1: editing a new line
std::vector<std::string> g_lines;
std::vector<int> g_matches;           // into g_names, for the first word of the input
std::string g_matchedFor;
int g_selected = -1;                  // the match Tab last put in the input, or -1
bool g_wasShown = false;
bool g_refocus = false;
bool g_scrollToEnd = false;
float g_heightShare = 0.45f;

std::string firstWord(const char* text)
{
    while (*text == ' ')
        ++text;
    const char* end = text;
    while (*end && *end != ' ')
        ++end;
    return std::string(text, end);
}

void updateMatches()
{
    if (g_selected >= 0)
        return;   // stepping through the matches with Tab: the list stays as it was
    const std::string word = firstWord(g_input);
    if (word == g_matchedFor)
        return;
    g_matchedFor = word;
    g_matches.clear();
    if (word.empty())
        return;
    for (size_t i = 0; i < g_names.size(); ++i)
        if (_strnicmp(g_names[i].name.c_str(), word.c_str(), word.size()) == 0)
            g_matches.push_back(static_cast<int>(i));
}

void replaceInput(ImGuiInputTextCallbackData* data, const std::string& text)
{
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, text.c_str());
}

int inputCallback(ImGuiInputTextCallbackData* data)
{
    if (data->EventFlag == ImGuiInputTextFlags_CallbackEdit)
    {
        g_selected = -1;   // typing starts a new completion
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion)
    {
        // Tab: complete the first word as far as all matches agree; once it can't go further,
        // step through the matches (Shift+Tab steps back).
        if (std::strchr(data->Buf, ' ') && g_selected < 0)
            return 0;
        if (g_selected >= 0 || g_matches.size() > 1)
        {
            std::string common;
            if (g_selected < 0)
            {
                common = g_names[g_matches[0]].name;
                for (size_t i = 1; i < g_matches.size(); ++i)
                {
                    const std::string& n = g_names[g_matches[i]].name;
                    size_t k = 0;
                    while (k < common.size() && k < n.size() && tolower(common[k]) == tolower(n[k]))
                        ++k;
                    common.resize(k);
                }
            }
            if (g_selected < 0 && common.size() > static_cast<size_t>(data->BufTextLen))
            {
                replaceInput(data, common);
                return 0;
            }
            const int count = static_cast<int>(g_matches.size());
            if (!count)
                return 0;
            const bool back = ImGui::GetIO().KeyShift;
            g_selected = g_selected < 0 ? (back ? count - 1 : 0) : (g_selected + (back ? count - 1 : 1)) % count;
            replaceInput(data, g_names[g_matches[g_selected]].name + " ");
            return 0;
        }
        if (g_matches.size() == 1)
            replaceInput(data, g_names[g_matches[0]].name + " ");
    }
    else if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !g_history.empty())
    {
        const int previous = g_historyPos;
        if (data->EventKey == ImGuiKey_UpArrow)
            g_historyPos = g_historyPos < 0 ? static_cast<int>(g_history.size()) - 1 : std::max(0, g_historyPos - 1);
        else if (data->EventKey == ImGuiKey_DownArrow && g_historyPos >= 0)
            g_historyPos = g_historyPos + 1 < static_cast<int>(g_history.size()) ? g_historyPos + 1 : -1;
        if (previous != g_historyPos)
            replaceInput(data, g_historyPos >= 0 ? g_history[g_historyPos] : std::string());
    }
    return 0;
}

void enter()
{
    std::string line(g_input);
    const size_t first = line.find_first_not_of(' ');
    g_input[0] = '\0';
    g_historyPos = -1;
    g_selected = -1;
    g_refocus = true;
    g_scrollToEnd = true;
    if (first == std::string::npos)
        return;
    line = line.substr(first);
    while (!line.empty() && line[line.size() - 1] == ' ')
        line.erase(line.size() - 1);
    if (g_history.empty() || g_history.back() != line)
        g_history.push_back(line);
    if (g_history.size() > 200)
        g_history.erase(g_history.begin());
    uiRunTyped(line);
}

bool containsNoCase(const std::string& text, const char* part)
{
    const size_t n = std::strlen(part);
    if (!n)
        return true;
    for (size_t i = 0; i + n <= text.size(); ++i)
        if (_strnicmp(text.c_str() + i, part, n) == 0)
            return true;
    return false;
}

// A log line's colour: lines entered here (the console echoes them), errors, warnings.
ImU32 lineColour(const std::string& line)
{
    for (size_t i = g_history.size(); i-- > 0 && i + 20 >= g_history.size();)
        if (line == g_history[i])
            return IM_COL32(110, 205, 200, 255);
    if (containsNoCase(line, "error") || containsNoCase(line, "unknown") || containsNoCase(line, "failed")
        || containsNoCase(line, "invalid") || containsNoCase(line, "not found"))
        return IM_COL32(240, 120, 110, 255);
    if (containsNoCase(line, "warning"))
        return IM_COL32(235, 200, 110, 255);
    return ImGui::GetColorU32(ImGuiCol_Text);
}

void drawCompletion(const ImVec2& at, float width)
{
    const int kShown = 12;
    if (g_matches.empty())
        return;
    const int count = static_cast<int>(g_matches.size());
    // The visible slice keeps the selected match in view.
    int start = 0;
    if (g_selected >= kShown)
        start = g_selected - kShown + 1;
    const bool exact = count == 1 && _stricmp(g_names[g_matches[0]].name.c_str(), g_matchedFor.c_str()) == 0;
    ImGui::SetNextWindowPos(at);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(width, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.97f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##completion", 0, flags))
    {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (ImGui::BeginTable("##matches", 2, ImGuiTableFlags_SizingFixedFit))
        {
            for (int i = start; i < count && i < start + kShown; ++i)
            {
                const Name& n = g_names[g_matches[i]];
                ImGui::TableNextRow();
                if (i == g_selected)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_HeaderActive));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(n.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", n.signature.c_str());
                if (n.help && (exact || i == g_selected || count <= 3))
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ImVec4(0.55f, 0.82f, 0.80f, 1.0f), "%s", n.help);
                }
            }
            ImGui::EndTable();
        }
        if (count > kShown)
            ImGui::TextDisabled("%d of %d   Tab / Shift+Tab steps through them", std::min(kShown, count - start), count);
    }
    ImGui::End();
}

} // namespace

float uiLayerGetConsoleHeight()
{
    return g_heightShare;
}

void uiLayerSetConsoleHeight(float share)
{
    share = std::max(0.2f, std::min(0.9f, share));
    if (share == g_heightShare)
        return;
    g_heightShare = share;
    uiSaveSettings();
}

bool uiConsoleShown()
{
    io::Console* const c = console();
    return c && c->getConsoleActive();
}

void uiConsoleDraw(bool menuOpen)
{
    io::Console* const c = console();
    if (!c)
        return;
    const bool opened = !g_wasShown;
    g_wasShown = true;
    if (opened)
    {
        g_refocus = true;
        g_scrollToEnd = true;
    }
    gatherNames();

    // The console's history, without its own prompt line (the last one getLines adds).
    g_lines.clear();
    c->getLines(c->getMaxHistorySize() + 1, g_lines);
    if (!g_lines.empty())
        g_lines.pop_back();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y * g_heightShare));
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoScrollbar;
    const bool open = ImGui::Begin("##console", 0, flags);
    ImGui::PopStyleVar();
    if (!open)
    {
        ImGui::End();
        return;
    }

    // Toolbar: title, filter, copy, line count, and the keys.
    std::vector<int> shown;
    shown.reserve(g_lines.size());
    for (size_t i = 0; i < g_lines.size(); ++i)
        if (containsNoCase(g_lines[i], g_filter))
            shown.push_back(static_cast<int>(i));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.43f, 0.80f, 0.78f, 1.0f), "Console");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    ImGui::InputTextWithHint("##filter", "filter the log", g_filter, sizeof(g_filter));
    if (g_filter[0])
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("x"))
            g_filter[0] = '\0';
        ImGui::SetItemTooltip("Clear the filter");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
    {
        std::string text;
        for (size_t i = 0; i < shown.size(); ++i)
            text += g_lines[shown[i]] + "\n";
        ImGui::SetClipboardText(text.c_str());
    }
    ImGui::SetItemTooltip("Copies the log lines shown");
    ImGui::SameLine();
    if (g_filter[0])
        ImGui::TextDisabled("%u of %u lines", static_cast<unsigned int>(shown.size()),
                            static_cast<unsigned int>(g_lines.size()));
    else
        ImGui::TextDisabled("%u lines", static_cast<unsigned int>(g_lines.size()));
    const char* const keys = "Tab complete   Up/Down history   Ctrl+V paste";
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()
                         + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(keys).x));
    ImGui::TextDisabled("%s", keys);

    // The log.
    const float footer = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginChild("##log", ImVec2(0.0f, -footer), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar))
    {
        const bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 1.0f));
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const std::string& line = g_lines[shown[i]];
                ImGui::PushStyleColor(ImGuiCol_Text, lineColour(line));
                ImGui::TextUnformatted(line.c_str());
                ImGui::PopStyleColor();
            }
        ImGui::PopStyleVar();
        if (g_scrollToEnd || atEnd)
            ImGui::SetScrollHereY(1.0f);
        g_scrollToEnd = false;
    }
    ImGui::EndChild();

    // The input line. It keeps the keyboard unless another field (the filter, the menu) has it.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.43f, 0.80f, 0.78f, 1.0f), ">");
    ImGui::SameLine();
    if (g_refocus || (!ImGui::IsAnyItemActive() && !menuOpen))
        ImGui::SetKeyboardFocusHere();
    g_refocus = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    const ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackEdit
        | ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
    updateMatches();
    if (ImGui::InputTextWithHint("##input", "type a command; Tab completes", g_input, sizeof(g_input),
                                 inputFlags, &inputCallback))
        enter();
    const ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f);
    const float width = ImGui::GetItemRectSize().x;
    const bool inputActive = ImGui::IsItemActive();
    ImGui::End();

    updateMatches();
    if (inputActive && g_input[0])
        drawCompletion(below, width);
}

void uiConsoleHidden()
{
    g_wasShown = false;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

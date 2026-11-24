// [RENDER] The level's weather (render milestone 18, docs/render/atmosphere.md#18-atmosphere-rework).
// Not part of retail BF2.
//
// At each level load the weather variables go back to their defaults (fair weather), then the
// level's file sets what it names: <GSModDirectory>/settings/eon64-weather/<level>.txt, outside
// this repository. A line is a variable without its "renderer." prefix and a value; '#' starts a
// comment. Lines before any section apply to every layer; "[<mode>]" applies to that game mode's
// layers and "[<mode> <size>]" to one layer, each in file order where the loaded layer matches:
//
//   weatherVisibility 40000
//   weatherAerosol 2
//   [gpm_cq 16]
//   cloudCoverage 0.7
//
// The water's type (render milestone 23, task 4) is here too: waterType, or its constituents
// waterChlorophyll, waterCdom and waterSediment after it; and the rivers' current (task 7):
// waterFlowTo, the bearing they run toward, and waterFlowSpeed.
//
// The values go through the console as typed lines, so they take the variables' own ranges, and
// the console changes them live afterwards (until the next load).
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/PluginEntry.h"
#include "../../RendDX9Abi/Frontier.h"
#include "RendDX9Base.h"
#include "../IO/Console/Console.h"
#include "../Game/IGame.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace dice {
namespace hfe {

extern IBase* mainConsole;

namespace dx9rend {

namespace {

// The weather variables and their defaults (fair: 25 km, continental, 1.5 km, the level's altitude
// from its bake, milestone 12's preset and coverage).
struct WeatherDefault
{
    const char* name;
    const char* value;
};
const WeatherDefault kDefaults[] = {
    { "weatherVisibility", "25000" }, { "weatherAerosol", "0" }, { "weatherMixing", "1500" },
    { "weatherAltitude", "-100000" }, { "cloudPreset", "0" },     { "cloudCoverage", "0.4" },
    { "waterType", "5" },   // coastal 1, clear coastal water (render milestone 23, task 4)
    { "waterFlowTo", "-1" }, { "waterFlowSpeed", "0" },   // still water (render milestone 23, task 7)
};
const int kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);

std::string g_source = "defaults (no level loaded)";

std::string lower(std::string s)
{
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    return s;
}

std::string trim(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// Variables a level's file may set that have no default of their own (waterType's sets them)
const char* const kAlso[] = { "waterChlorophyll", "waterCdom", "waterSediment" };

bool isWeatherVariable(const std::string& name)
{
    for (int i = 0; i < kDefaultCount; ++i)
        if (name == kDefaults[i].name)
            return true;
    for (size_t i = 0; i < sizeof(kAlso) / sizeof(kAlso[0]); ++i)
        if (name == kAlso[i])
            return true;
    return false;
}

void set(const std::string& name, const std::string& value)
{
    io::Console* const c = reinterpret_cast<io::Console*>(mainConsole);
    if (!c)
        return;
    std::string output;
    c->executeLine("renderer." + name + " " + value + "\n", output, false, true, false, false, true);
}

} // namespace

void rendererWeatherLoad()
{
    for (int i = 0; i < kDefaultCount; ++i)
        set(kDefaults[i].name, kDefaults[i].value);

    std::string mod, level, mode;
    int size = 0;
    readStringSettingDefault("GSModDirectory", "mods/bf2", mod);
    IGame* const host = classManager
        ? static_cast<IGame*>(classManager->getSingleton(renddx9abi::HostString("Game")))
        : 0;
    if (host)
    {
        level = host->getCurrentMapName();
        mode = host->getCurrentMapGameMode();
        size = host->getCurrentMapSize();
    }
    if (level.empty())
        readStringSetting("GSLoadLevel", level);
    level = lower(level);
    mode = lower(mode);
    if (level.empty())
    {
        g_source = "defaults (no level name)";
        return;
    }
    if (!mod.empty() && mod[mod.size() - 1] != '/' && mod[mod.size() - 1] != '\\')
        mod += '/';
    const std::string path = mod + "settings/eon64-weather/" + level + ".txt";
    std::ostringstream layer;
    layer << mode << " " << size;

    std::FILE* in = std::fopen(path.c_str(), "r");
    if (!in)
    {
        g_source = "defaults (no " + path + ")";
        ::renddx9abi::logf("[weather] %s, layer %s: %s", level.c_str(), layer.str().c_str(), g_source.c_str());
        return;
    }
    std::ostringstream applied;
    applied << path << ", layer " << layer.str() << ":";
    bool active = true;
    std::string section = "all layers";
    int lineNumber = 0;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), in))
    {
        ++lineNumber;
        std::string line = buffer;
        const size_t hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        line = trim(line);
        if (line.empty())
            continue;
        if (line[0] == '[')
        {
            const size_t close = line.find(']');
            std::istringstream header(lower(line.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            std::string sectionMode;
            int sectionSize = 0;
            header >> sectionMode;
            const bool hasSize = static_cast<bool>(header >> sectionSize);
            active = sectionMode == mode && (!hasSize || sectionSize == size);
            section = trim(line);
            continue;
        }
        if (!active)
            continue;
        std::istringstream fields(line);
        std::string name, value;
        fields >> name >> value;
        if (name.compare(0, 9, "renderer.") == 0)
            name.erase(0, 9);
        if (!isWeatherVariable(name) || value.empty())
        {
            ::renddx9abi::logf("[weather] %s:%d: not a weather variable or no value: %s", path.c_str(), lineNumber,
                               line.c_str());
            continue;
        }
        set(name, value);
        applied << " " << name << " " << value << " (" << section << ")";
    }
    std::fclose(in);
    g_source = applied.str();
    ::renddx9abi::logf("[weather] %s", g_source.c_str());
}

const char* rendererWeatherSource()
{
    return g_source.c_str();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

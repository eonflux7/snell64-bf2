// [RENDER] Which detail textures are hex-tiled (render milestone 19); see HexTilingList.h.
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/PluginAbi.h"
#include "../../RendDX9Abi/Frontier.h"
#include "../RendDX9/RendDX9Base.h"
#include "HexTilingList.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <string>

namespace dice {
namespace hfe {
namespace geom {

namespace {

bool g_loaded = false;
std::set<std::string> g_lists[2];
// [static hex] entries with strips: up to four (from, to, steps) V ranges each; and the whole
// sheet's steps.
std::map<std::string, std::vector<float> > g_strips;
std::map<std::string, float> g_sheetSteps;

// The file name without directory or extension, lower case.
std::string textureKey(const std::string& path)
{
    std::string::size_type start = path.find_last_of("/\\");
    start = (start == std::string::npos) ? 0 : start + 1;
    std::string::size_type end = path.find('.', start);
    if (end == std::string::npos)
        end = path.size();
    std::string key = path.substr(start, end - start);
    for (std::size_t i = 0; i != key.size(); ++i)
        key[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[i])));
    return key;
}

std::string trim(const std::string& s)
{
    std::string::size_type a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

} // anonymous namespace

void hexTilingListLoad()
{
    g_loaded = true;
    g_lists[0].clear();
    g_lists[1].clear();
    g_strips.clear();
    g_sheetSteps.clear();

    std::string mod;
    dx9rend::readStringSettingDefault("GSModDirectory", "mods/bf2", mod);
    if (!mod.empty() && mod[mod.size() - 1] != '/' && mod[mod.size() - 1] != '\\')
        mod += '/';
    const std::string path = mod + "settings/eon64-hextiling.txt";
    std::FILE* in = std::fopen(path.c_str(), "r");
    if (!in)
    {
        ::renddx9abi::logf("[hextiling] no %s: every terrain layer hex-tiled, no static mesh", path.c_str());
        return;
    }
    int section = -1;
    int lineNumber = 0;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), in))
    {
        ++lineNumber;
        std::string line = buffer;
        const std::string::size_type hash = line.find('#');
        if (hash != std::string::npos)
            line.erase(hash);
        line = trim(line);
        if (line.empty())
            continue;
        if (line[0] == '[')
        {
            const std::string header = textureKey(line);   // lower case; no '/', '\\' or '.' in a header
            section = header == "[terrain grid]" ? kHexTerrainGrid
                    : header == "[static hex]"   ? kHexStaticHex
                    : -1;
            if (section < 0)
                ::renddx9abi::logf("[hextiling] %s:%d: unknown section %s", path.c_str(), lineNumber, line.c_str());
            continue;
        }
        if (section < 0)
            continue;
        std::istringstream fields(line);
        std::string name, range;
        fields >> name;
        const std::string key = textureKey(name);
        g_lists[section].insert(key);
        std::vector<float> strips;
        while (section == kHexStaticHex && fields >> range)
        {
            // "/N": shift steps, for the whole sheet (alone) or the range before it
            const std::string::size_type slash = range.find('/');
            const float steps = slash == std::string::npos ? 0.0f
                              : static_cast<float>(std::atof(range.substr(slash + 1).c_str()));
            if (slash == 0)
            {
                g_sheetSteps[key] = steps;
                continue;
            }
            range = range.substr(0, slash);
            const std::string::size_type dash = range.find('-', 1);
            const float from = static_cast<float>(std::atof(range.substr(0, dash).c_str()));
            const float to = dash == std::string::npos ? -1.0f
                           : static_cast<float>(std::atof(range.substr(dash + 1).c_str()));
            if (dash == std::string::npos || !(to > from) || strips.size() == 12)
            {
                ::renddx9abi::logf("[hextiling] %s:%d: not a V range (from-to[/N], at most four): %s", path.c_str(),
                                   lineNumber, range.c_str());
                continue;
            }
            strips.push_back(from);
            strips.push_back(to);
            strips.push_back(steps);
        }
        if (!strips.empty())
            g_strips[key] = strips;
    }
    std::fclose(in);
    ::renddx9abi::logf("[hextiling] %s: %u terrain textures on the grid, %u static mesh textures hex-tiled "
                       "(%u by strips)", path.c_str(), static_cast<unsigned int>(g_lists[kHexTerrainGrid].size()),
                       static_cast<unsigned int>(g_lists[kHexStaticHex].size()),
                       static_cast<unsigned int>(g_strips.size()));
}

bool hexTilingListed(HexTilingSection section, const std::string& path)
{
    if (!g_loaded)
        hexTilingListLoad();
    return g_lists[section].count(textureKey(path)) != 0;
}

void hexTilingStatic(const std::string& path, float out[16])
{
    for (int i = 0; i != 16; ++i)
        out[i] = 0.0f;
    for (int i = 4; i != 12; i += 2)
        out[i] = 1.0f;   // an empty range: from 1 to 0
    if (path.empty() || !hexTilingListed(kHexStaticHex, path))
        return;
    const std::string key = textureKey(path);
    const std::map<std::string, std::vector<float> >::const_iterator found = g_strips.find(key);
    if (found == g_strips.end())
    {
        const std::map<std::string, float>::const_iterator steps = g_sheetSteps.find(key);
        out[0] = 1.0f;
        out[1] = steps == g_sheetSteps.end() ? 0.0f : steps->second;
        return;
    }
    out[0] = 2.0f;
    for (std::size_t i = 0; i * 3 < found->second.size(); ++i)
    {
        out[4 + 2 * i] = found->second[3 * i];
        out[5 + 2 * i] = found->second[3 * i + 1];
        out[12 + i] = found->second[3 * i + 2];
    }
}

} // namespace geom
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

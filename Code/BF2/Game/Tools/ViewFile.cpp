// [TOOLS] A map's view file; see ViewFile.h and docs/lanes/tools.md#1-views for the format:
//
//   # views for muttrah_city_2
//   view market   pos=812.4,62.0,-311.9 yaw=134.5 pitch=-8 fov=75 | rooftop alpha fences
//     run renderer.sunShadowDebug 1
//   set default market airfield
//   path tour
//     key 0  pos=812.4,62.0,-311.9 yaw=134.5 pitch=-8 fov=75
//     key 12 pos=-1203.0,48.5,902.2 yaw=12.0 pitch=-3

#include "ViewFile.h"

#if !DEDICATED

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace dice {
namespace hfe {

namespace {

std::string trim(const std::string& s)
{
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::vector<std::string> words(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string word;
    while (in >> word)
        out.push_back(word);
    return out;
}

bool parseFloat(const std::string& text, float& value)
{
    if (text.empty())
        return false;
    char* end = 0;
    value = static_cast<float>(std::strtod(text.c_str(), &end));
    return end && *end == '\0';
}

bool parsePos(const std::string& text, float& x, float& y, float& z)
{
    const size_t a = text.find(',');
    const size_t b = a == std::string::npos ? a : text.find(',', a + 1);
    if (b == std::string::npos)
        return false;
    return parseFloat(text.substr(0, a), x) && parseFloat(text.substr(a + 1, b - a - 1), y)
        && parseFloat(text.substr(b + 1), z);
}

std::string lineProblem(int index, const std::string& what)
{
    std::ostringstream out;
    out << "line " << (index + 1) << ": " << what;
    return out.str();
}

// The camera fields of a view or key line into the given outputs; problems are reported for line i.
// Returns false when pos is missing.
struct CameraFields
{
    float x, y, z, yaw, pitch, fov;
    bool hasFov;
};

bool parseCameraFields(const std::vector<std::pair<std::string, std::string> >& fields, int i,
                       CameraFields& c, std::vector<std::string>& problems);

// The key=value words of a line from word `from` on; a word without '=' has an empty value.
void splitFields(const std::vector<std::string>& w, size_t from,
                 std::vector<std::pair<std::string, std::string> >& fields)
{
    fields.clear();
    for (size_t i = from; i < w.size(); ++i)
    {
        const size_t eq = w[i].find('=');
        if (eq == std::string::npos)
            fields.push_back(std::make_pair(w[i], std::string()));
        else
            fields.push_back(std::make_pair(w[i].substr(0, eq), w[i].substr(eq + 1)));
    }
}

// A view line's parts: the keyword and name, the key=value fields in order, the note.
struct HeaderParts
{
    std::string name;
    std::vector<std::pair<std::string, std::string> > fields;
    std::string note;
    bool hasNote;
};

bool splitHeader(const std::string& line, HeaderParts& parts, const char* keyword = "view")
{
    const size_t bar = line.find('|');
    parts.hasNote = bar != std::string::npos;
    parts.note = parts.hasNote ? trim(line.substr(bar + 1)) : std::string();
    const std::vector<std::string> w = words(parts.hasNote ? line.substr(0, bar) : line);
    if (w.size() < 2 || w[0] != keyword)
        return false;
    parts.name = w[1];
    splitFields(w, 2, parts.fields);
    return true;
}

bool parseCameraFields(const std::vector<std::pair<std::string, std::string> >& fields, int i,
                       CameraFields& c, std::vector<std::string>& problems)
{
    c.x = c.y = c.z = c.yaw = c.pitch = c.fov = 0.0f;
    c.hasFov = false;
    bool hasPos = false;
    for (size_t f = 0; f < fields.size(); ++f)
    {
        const std::string& key = fields[f].first;
        const std::string& value = fields[f].second;
        bool ok = true;
        if (key == "pos")
            ok = hasPos = parsePos(value, c.x, c.y, c.z);
        else if (key == "yaw")
            ok = parseFloat(value, c.yaw);
        else if (key == "pitch")
            ok = parseFloat(value, c.pitch);
        else if (key == "fov")
            ok = c.hasFov = parseFloat(value, c.fov);
        else
            problems.push_back(lineProblem(i, "unknown field " + key + " (kept)"));
        if (!ok)
            problems.push_back(lineProblem(i, "bad value in " + key + "=" + value));
    }
    return hasPos;
}

std::string joinHeader(const HeaderParts& parts, const char* keyword = "view")
{
    std::string line = std::string(keyword) + " " + parts.name;
    for (size_t i = 0; i < parts.fields.size(); ++i)
    {
        line += ' ';
        line += parts.fields[i].first;
        if (!parts.fields[i].second.empty())
            line += '=' + parts.fields[i].second;
    }
    if (parts.hasNote && !parts.note.empty())
        line += " | " + parts.note;
    return line;
}

void setField(HeaderParts& parts, const std::string& key, const std::string& value)
{
    for (size_t i = 0; i < parts.fields.size(); ++i)
    {
        if (parts.fields[i].first == key)
        {
            parts.fields[i].second = value;
            return;
        }
    }
    parts.fields.push_back(std::make_pair(key, value));
}

} // namespace

ViewFile::ViewFile() : m_exists(false), m_crlf(false) { }

std::string ViewFile::readFile(const std::string& path, bool& exists)
{
    exists = false;
    std::string text;
    std::FILE* in = path.empty() ? 0 : std::fopen(path.c_str(), "rb");
    if (!in)
        return text;
    exists = true;
    char buffer[4096];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), in)) > 0)
        text.append(buffer, n);
    std::fclose(in);
    return text;
}

bool ViewFile::changedOnDisk(const std::string& path) const
{
    if (path != m_path)
        return true;
    bool exists = false;
    const std::string raw = readFile(path, exists);
    return exists != m_exists || raw != m_raw;
}

bool ViewFile::load(const std::string& path)
{
    m_path = path;
    m_raw = readFile(path, m_exists);
    m_crlf = m_raw.find("\r\n") != std::string::npos;
    m_lines.clear();
    size_t start = 0;
    while (start < m_raw.size())
    {
        size_t end = m_raw.find('\n', start);
        if (end == std::string::npos)
            end = m_raw.size();
        std::string line = m_raw.substr(start, end - start);
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        m_lines.push_back(line);
        start = end + 1;
    }
    parse();
    return true;
}

void ViewFile::parse()
{
    m_views.clear();
    m_sets.clear();
    m_paths.clear();
    m_problems.clear();
    // run lines belong to the view above them, key lines to the path above them.
    enum { None, InView, InPath } block = None;
    for (int i = 0; i < static_cast<int>(m_lines.size()); ++i)
    {
        const std::string line = trim(m_lines[i]);
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> w = words(line);
        if (w[0] == "view")
        {
            block = None;
            HeaderParts parts;
            if (!splitHeader(line, parts) || !validName(parts.name))
            {
                m_problems.push_back(lineProblem(i, "view needs a name"));
                continue;
            }
            if (find(parts.name))
            {
                m_problems.push_back(lineProblem(i, "view " + parts.name + " is defined twice; "
                                                    "the first one is used"));
                continue;
            }
            CameraFields c;
            if (!parseCameraFields(parts.fields, i, c, m_problems))
            {
                m_problems.push_back(lineProblem(i, "view " + parts.name + " has no pos"));
                continue;
            }
            ViewDef v;
            v.name = parts.name;
            v.x = c.x;
            v.y = c.y;
            v.z = c.z;
            v.yaw = c.yaw;
            v.pitch = c.pitch;
            v.fov = c.fov;
            v.hasFov = c.hasFov;
            v.note = parts.note;
            v.header = i;
            m_views.push_back(v);
            block = InView;
        }
        else if (w[0] == "run")
        {
            const std::string command = trim(line.substr(3));
            if (block != InView || command.empty())
            {
                m_problems.push_back(lineProblem(i, block != InView ? "run line outside a view"
                                                                    : "empty run line"));
                continue;
            }
            m_views.back().runs.push_back(command);
            m_views.back().runLines.push_back(i);
        }
        else if (w[0] == "path")
        {
            block = None;
            HeaderParts parts;
            if (!splitHeader(line, parts, "path") || !validName(parts.name))
            {
                m_problems.push_back(lineProblem(i, "path needs a name"));
                continue;
            }
            if (!parts.fields.empty())
                m_problems.push_back(lineProblem(i, "path takes no fields (kept)"));
            if (findPath(parts.name))
            {
                m_problems.push_back(lineProblem(i, "path " + parts.name + " is defined twice; "
                                                    "the first one is used"));
                continue;
            }
            ViewPath p;
            p.name = parts.name;
            p.note = parts.note;
            p.header = i;
            m_paths.push_back(p);
            block = InPath;
        }
        else if (w[0] == "key")
        {
            if (block != InPath)
            {
                m_problems.push_back(lineProblem(i, "key line outside a path"));
                continue;
            }
            float time = 0.0f;
            if (w.size() < 2 || !parseFloat(w[1], time) || time < 0.0f)
            {
                m_problems.push_back(lineProblem(i, "key needs a time in seconds"));
                continue;
            }
            std::vector<std::pair<std::string, std::string> > fields;
            splitFields(w, 2, fields);
            CameraFields c;
            if (!parseCameraFields(fields, i, c, m_problems))
            {
                m_problems.push_back(lineProblem(i, "key has no pos"));
                continue;
            }
            ViewPath& p = m_paths.back();
            if (!p.keys.empty() && time <= p.keys.back().time)
            {
                m_problems.push_back(lineProblem(i, "key time " + w[1] + " is not after the "
                                                    "previous key's; skipped"));
                continue;
            }
            PathKey k;
            k.time = time;
            k.x = c.x;
            k.y = c.y;
            k.z = c.z;
            k.yaw = c.yaw;
            k.pitch = c.pitch;
            k.fov = c.fov;
            k.hasFov = c.hasFov;
            k.line = i;
            p.keys.push_back(k);
        }
        else if (w[0] == "set")
        {
            if (w.size() < 2 || !validName(w[1]))
            {
                m_problems.push_back(lineProblem(i, "set needs a name"));
                continue;
            }
            ViewSet s;
            s.name = w[1];
            s.views.assign(w.begin() + 2, w.end());
            s.line = i;
            m_sets.push_back(s);
        }
        else
        {
            m_problems.push_back(lineProblem(i, "unknown line: " + w[0]));
        }
    }
    // Sets may name views defined further down, so they are checked once all views are known.
    for (size_t s = 0; s < m_sets.size(); ++s)
        for (size_t v = 0; v < m_sets[s].views.size(); ++v)
            if (!find(m_sets[s].views[v]))
                m_problems.push_back(lineProblem(m_sets[s].line, "set " + m_sets[s].name
                                                 + " names unknown view " + m_sets[s].views[v]));
}

const ViewDef* ViewFile::find(const std::string& name) const
{
    for (size_t i = 0; i < m_views.size(); ++i)
        if (m_views[i].name == name)
            return &m_views[i];
    return 0;
}

const ViewPath* ViewFile::findPath(const std::string& name) const
{
    for (size_t i = 0; i < m_paths.size(); ++i)
        if (m_paths[i].name == name)
            return &m_paths[i];
    return 0;
}

bool ViewFile::validName(const std::string& name)
{
    return !name.empty() && name.find_first_of(" \t\r\n|#=") == std::string::npos;
}

std::string ViewFile::formatNumber(float value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos)
    {
        while (text[text.size() - 1] == '0')
            text.erase(text.size() - 1);
        if (text[text.size() - 1] == '.')
            text.erase(text.size() - 1);
    }
    if (text == "-0")
        text = "0";
    return text;
}

void ViewFile::saveView(const std::string& name, float x, float y, float z, float yaw, float pitch,
                        float fov, const std::string& note)
{
    const ViewDef* existing = find(name);
    HeaderParts parts;
    parts.hasNote = false;
    if (existing)
        splitHeader(trim(m_lines[existing->header]), parts);
    else
        parts.name = name;
    setField(parts, "pos", formatNumber(x, 2) + "," + formatNumber(y, 2) + "," + formatNumber(z, 2));
    setField(parts, "yaw", formatNumber(yaw, 2));
    setField(parts, "pitch", formatNumber(pitch, 2));
    setField(parts, "fov", formatNumber(fov, 1));
    if (!note.empty())
    {
        parts.note = note;
        parts.hasNote = true;
    }
    const std::string line = joinHeader(parts);

    if (existing)
    {
        m_lines[existing->header] = line;
    }
    else
    {
        // After the last view's block, so new views stay above the sets; at the end otherwise.
        int at = static_cast<int>(m_lines.size());
        if (!m_views.empty())
        {
            const ViewDef& last = m_views.back();
            at = (last.runLines.empty() ? last.header : last.runLines.back()) + 1;
        }
        else if (!m_sets.empty())
        {
            at = m_sets.front().line;
        }
        m_lines.insert(m_lines.begin() + at, line);
    }
    parse();
}

bool ViewFile::deleteView(const std::string& name)
{
    const ViewDef* view = find(name);
    if (!view)
        return false;
    std::vector<int> remove(view->runLines);
    remove.push_back(view->header);
    for (size_t s = 0; s < m_sets.size(); ++s)
    {
        ViewSet& set = m_sets[s];
        std::vector<std::string> kept;
        for (size_t v = 0; v < set.views.size(); ++v)
            if (set.views[v] != name)
                kept.push_back(set.views[v]);
        if (kept.size() == set.views.size())
            continue;
        if (kept.empty())
        {
            remove.push_back(set.line);
            continue;
        }
        std::string line = "set " + set.name;
        for (size_t v = 0; v < kept.size(); ++v)
            line += " " + kept[v];
        m_lines[set.line] = line;
    }
    // Highest index first, so the lower ones stay valid.
    for (size_t i = 0; i < remove.size(); ++i)
        for (size_t j = i + 1; j < remove.size(); ++j)
            if (remove[j] > remove[i])
                std::swap(remove[i], remove[j]);
    for (size_t i = 0; i < remove.size(); ++i)
        m_lines.erase(m_lines.begin() + remove[i]);
    parse();
    return true;
}

namespace {

int blockEnd(const ViewDef& view)
{
    return view.runLines.empty() ? view.header : view.runLines.back();
}

} // namespace

bool ViewFile::moveView(const std::string& name, int delta)
{
    size_t index = m_views.size();
    for (size_t i = 0; i < m_views.size(); ++i)
        if (m_views[i].name == name)
            index = i;
    if (index == m_views.size() || delta == 0)
        return false;
    const size_t other = delta < 0 ? index - 1 : index + 1;
    if ((delta < 0 && index == 0) || other >= m_views.size())
        return false;
    const ViewDef& first = m_views[delta < 0 ? other : index];
    const ViewDef& second = m_views[delta < 0 ? index : other];
    const std::vector<std::string>::iterator begin = m_lines.begin();
    std::vector<std::string> lines(begin, begin + first.header);
    lines.insert(lines.end(), begin + second.header, begin + blockEnd(second) + 1);
    lines.insert(lines.end(), begin + blockEnd(first) + 1, begin + second.header);
    lines.insert(lines.end(), begin + first.header, begin + blockEnd(first) + 1);
    lines.insert(lines.end(), begin + blockEnd(second) + 1, m_lines.end());
    m_lines.swap(lines);
    parse();
    return true;
}

bool ViewFile::setNote(const std::string& name, const std::string& note)
{
    const ViewDef* view = find(name);
    if (!view)
        return false;
    HeaderParts parts;
    splitHeader(trim(m_lines[view->header]), parts);
    parts.note = trim(note);
    parts.hasNote = !parts.note.empty();
    m_lines[view->header] = joinHeader(parts);
    parse();
    return true;
}

void ViewFile::setSet(const std::string& name, const std::vector<std::string>& views)
{
    std::string line = "set " + name;
    for (size_t v = 0; v < views.size(); ++v)
        line += " " + views[v];
    for (size_t s = 0; s < m_sets.size(); ++s)
    {
        if (m_sets[s].name != name)
            continue;
        if (views.empty())
            m_lines.erase(m_lines.begin() + m_sets[s].line);
        else
            m_lines[m_sets[s].line] = line;
        parse();
        return;
    }
    if (views.empty())
        return;
    const int at = m_sets.empty() ? static_cast<int>(m_lines.size()) : m_sets.back().line + 1;
    m_lines.insert(m_lines.begin() + at, line);
    parse();
}

void ViewFile::saveKey(const std::string& path, float time, float x, float y, float z, float yaw,
                       float pitch, float fov)
{
    const std::string line = "  key " + formatNumber(time, 2) + " pos=" + formatNumber(x, 2) + ","
        + formatNumber(y, 2) + "," + formatNumber(z, 2) + " yaw=" + formatNumber(yaw, 2)
        + " pitch=" + formatNumber(pitch, 2)
        + (fov > 0.0f ? " fov=" + formatNumber(fov, 1) : std::string());
    const ViewPath* existing = findPath(path);
    if (existing)
    {
        int at = existing->keys.empty() ? existing->header + 1 : existing->keys.back().line + 1;
        for (size_t k = 0; k < existing->keys.size(); ++k)
        {
            const PathKey& key = existing->keys[k];
            if (formatNumber(key.time, 2) == formatNumber(time, 2))
            {
                m_lines[key.line] = line;
                parse();
                return;
            }
            if (key.time > time)
            {
                at = key.line;
                break;
            }
        }
        m_lines.insert(m_lines.begin() + at, line);
        parse();
        return;
    }
    // A new path goes after the last path, else after the last view, else above the sets.
    int at = static_cast<int>(m_lines.size());
    if (!m_paths.empty())
    {
        const ViewPath& last = m_paths.back();
        at = (last.keys.empty() ? last.header : last.keys.back().line) + 1;
    }
    else if (!m_views.empty())
    {
        const ViewDef& last = m_views.back();
        at = (last.runLines.empty() ? last.header : last.runLines.back()) + 1;
    }
    else if (!m_sets.empty())
    {
        at = m_sets.front().line;
    }
    m_lines.insert(m_lines.begin() + at, line);
    m_lines.insert(m_lines.begin() + at, "path " + path);
    parse();
}

bool ViewFile::write(std::string& error)
{
    std::string text;
    const char* const eol = m_crlf ? "\r\n" : "\n";
    for (size_t i = 0; i < m_lines.size(); ++i)
        text += m_lines[i] + eol;
    std::FILE* out = m_path.empty() ? 0 : std::fopen(m_path.c_str(), "wb");
    if (!out)
    {
        error = "cannot write " + m_path;
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    std::fclose(out);
    if (!ok)
    {
        error = "short write to " + m_path;
        return false;
    }
    m_raw = text;
    m_exists = true;
    return true;
}

} // namespace hfe
} // namespace dice

#endif // !DEDICATED

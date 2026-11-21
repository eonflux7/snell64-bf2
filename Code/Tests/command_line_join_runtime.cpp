// [TEMP] The argv join of the console entry points (CTest bf2_command_line_join_check) against the
// recovered SimpleParser. Retail fix "The Linux server keeps command-line arguments that contain
// spaces" (docs/retail-fixes.md): the joined line must parse exactly as retail's line for every
// input, except that a value which retail cuts at its first whitespace arrives whole.
//
// retailJoin models the Linux ia-32 server main (ELF 0x8052C62 to 0x8052CA6): argv[1..] streamed
// with one ' ' between arguments and no quoting.
#include "../BF2/Game/Main/CommandLineJoin.h"
#include "../BF2/Game/Utility/SimpleParser.h"

#include <cstdio>
#include <list>
#include <string>
#include <utility>
#include <vector>

namespace {

typedef std::vector<std::string> Args;
typedef std::list<std::pair<int, std::string> > Keys;

unsigned checks, failures;

void check(bool value, const std::string& label)
{
    ++checks;
    if (!value)
    {
        ++failures;
        std::printf("FAIL %s\n", label.c_str());
    }
}

std::string join(const Args& args)
{
    std::vector<const char*> argv(1, "bf2");
    for (std::size_t i = 0; i < args.size(); ++i)
        argv.push_back(args[i].c_str());
    return dice::hfe::commandLine::joinArgs(static_cast<int>(argv.size()), &argv[0]);
}

std::string retailJoin(const Args& args)
{
    std::string out;
    for (std::size_t i = 0; i < args.size(); ++i)
        out += (i ? " " : "") + args[i];
    return out;
}

// Registers a subset of BF2Engine::parseParameters' options (and one dashed port option name),
// then parses as parseParameters does: an empty line is not parsed at all.
bool parse(const std::string& line, Keys& keys)
{
    keys.clear();
    if (line.empty())
        return false;
    dice::hfe::SimpleParser parser('+');
    parser.add(58, "?", "");
    parser.add(0, "dedicated", "");
    parser.add(8, "config", "");
    parser.add(9, "mapList", "");
    parser.add(66, "port", "");
    parser.add(5, "password", "");
    parser.add(46, "modPath", "");
    parser.add(21, "noSound", "");
    parser.add(11, "loadLevel", "");
    parser.add(4, "playerName", "");
    parser.add(45, "gameMode", "");
    parser.add(86, "gamespyRedirectstage-net", "");
    return parser.parseString(line, keys);
}

std::string show(const Keys& keys)
{
    std::string out;
    for (Keys::const_iterator it = keys.begin(); it != keys.end(); ++it)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "%d", it->first);
        out += std::string(out.empty() ? "" : " ") + id + "=[" + it->second + "]";
    }
    return out;
}

bool isSpace(char c)
{
    return dice::hfe::commandLine::isParserSpace(c);
}

// One table row: the arguments, the expected joined line, and whether the parse may differ from
// retail's (only when retail cuts a value passed as one argument).
struct Row
{
    const char* label;
    Args args;
    std::string line;
    bool fixesRetailCut;
};

Args A(const char* a, const char* b = 0, const char* c = 0, const char* d = 0, const char* e = 0,
       const char* f = 0)
{
    const char* all[] = { a, b, c, d, e, f };
    Args args;
    for (int i = 0; i < 6 && all[i]; ++i)
        args.push_back(all[i]);
    return args;
}

void checkRow(const Row& row)
{
    const std::string line = join(row.args);
    check(line == row.line, std::string(row.label) + ": line [" + line + "], expected [" + row.line
                                + "]");

    Keys retail, ours;
    const bool retailParsed = parse(retailJoin(row.args), retail);
    const bool oursParsed = parse(line, ours);
    if (row.fixesRetailCut)
        check(show(retail) != show(ours),
              std::string(row.label) + ": expected a whole value where retail cuts, got "
                  + show(ours));
    else
        check(retailParsed == oursParsed && show(retail) == show(ours),
              std::string(row.label) + ": parse " + show(ours) + ", retail " + show(retail));
}

// Every parse difference must be a key whose value is a whole argument (one with whitespace and no
// quote) that retail cut at its first whitespace.
bool onlyCutsDiffer(const Args& args)
{
    Keys retail, ours;
    const bool retailParsed = parse(retailJoin(args), retail);
    const bool oursParsed = parse(join(args), ours);
    if (retailParsed != oursParsed || retail.size() != ours.size())
        return false;
    for (Keys::const_iterator r = retail.begin(), o = ours.begin(); r != retail.end(); ++r, ++o)
    {
        if (r->first != o->first)
            return false;
        if (r->second == o->second)
            continue;
        bool wholeArgument = false;
        for (std::size_t i = 0; i < args.size(); ++i)
            wholeArgument = wholeArgument || args[i] == o->second;
        std::string::size_type cut = 0;
        while (cut < o->second.size() && !isSpace(o->second[cut]))
            ++cut;
        if (!wholeArgument || o->second.find('"') != std::string::npos
            || cut == o->second.size() || r->second != o->second.substr(0, cut))
            return false;
    }
    return true;
}

} // namespace

int main()
{
    const Row rows[] = {
        { "plain values", A("+config", "sv.con", "+modPath", "mods/bf2"),
          "+config sv.con +modPath mods/bf2", false },
        { "value with a space", A("+config", "/srv/My Server/sv.con"),
          "+config \"/srv/My Server/sv.con\"", true },
        { "value with a tab", A("+config", "/srv/My\tServer/sv.con"),
          "+config \"/srv/My\tServer/sv.con\"", true },
        { "value with a newline", A("+playerName", "a\nb"), "+playerName \"a\nb\"", true },
        { "value with C++ in it", A("+config", "/srv/C++ Stuff/sv.con"),
          "+config \"/srv/C++ Stuff/sv.con\"", true },
        { "macOS app paths", A("+modPath", "mods/bf2", "+config",
                               "/Users/x/Library/Application Support/OpenBF2/sv.con", "+noSound",
                               "1"),
          "+modPath mods/bf2 +config \"/Users/x/Library/Application Support/OpenBF2/sv.con\" "
          "+noSound 1", true },
        { "own quotes in one argument", A("+config", "\"/srv/My Server/sv.con\""),
          "+config \"/srv/My Server/sv.con\"", false },
        { "own quotes split by the shell", A("+config", "\"/srv/My", "Server/sv.con\""),
          "+config \"/srv/My Server/sv.con\"", false },
        { "own quotes in three pieces", A("+config", "\"/srv/My", "Big Server", "x/sv.con\""),
          "+config \"/srv/My Big Server x/sv.con\"", false },
        { "key and quoted value in one argument", A("+config \"/srv/My Server/sv.con\""),
          "+config \"/srv/My Server/sv.con\"", false },
        { "several options in one argument", A("+modPath mods/bf2 +gameMode gpm_cq"),
          "+modPath mods/bf2 +gameMode gpm_cq", false },
        { "whole line as one argument", A("+dedicated 1 +config sv.con +port 16568"),
          "+dedicated 1 +config sv.con +port 16568", false },
        { "key and value in one argument", A("+loadLevel strike_at_karkand"),
          "+loadLevel strike_at_karkand", false },
        { "text before an option", A("x +port 16568"), "x +port 16568", false },
        { "option at the end of an argument", A("a b +dedicated", "1"), "a b +dedicated 1", false },
        { "dashed option name", A("+gamespyRedirectstage-net 10.0.0.1 +port 1"),
          "+gamespyRedirectstage-net 10.0.0.1 +port 1", false },
        { "value holding an option", A("+password", "a +port 1"), "+password a +port 1", false },
        { "value starting with +", A("+password", "+abc def"), "+password +abc def", false },
        { "stray quote earlier", A("+playerName", "a\"b", "+config", "/a b/c"),
          "+playerName a\"b +config /a b/c", false },
        { "open quote earlier", A("+config", "\"/a", "b c", "d\""), "+config \"/a b c d\"", false },
        { "empty value", A("+password", "", "+port", "16568"), "+password  +port 16568", false },
        { "empty value at the end", A("+password", ""), "+password ", false },
        { "empty first argument", A("", "+port", "16568"), " +port 16568", false },
        { "only an empty argument", A(""), "", false },
        { "unknown option, spaced value", A("+confg", "a b"), "+confg \"a b\"", false },
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        checkRow(rows[i]);
    check(join(Args()).empty(), "no arguments: empty line");

    // Deterministic sweep over short arguments built from option names, whitespace, quotes and
    // plus signs: no difference from retail's parse other than whole values where retail cuts.
    const char* const pieces[] = { "+config", "+port", "+?", "+", "config", "port", "x", " ",
                                   "\t", "\"", "C++", "-", "", "+port ", " +config" };
    const unsigned pieceCount = sizeof(pieces) / sizeof(pieces[0]);
    unsigned seed = 12345, swept = 0, differing = 0, violations = 0;
    for (int n = 0; n < 200000; ++n)
    {
        seed = seed * 1103515245u + 12345u;
        Args args((seed >> 16) % 4 + 1);
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            seed = seed * 1103515245u + 12345u;
            const unsigned count = (seed >> 16) % 4;
            for (unsigned k = 0; k < count; ++k)
            {
                seed = seed * 1103515245u + 12345u;
                args[i] += pieces[(seed >> 16) % pieceCount];
            }
        }
        ++swept;
        Keys retail, ours;
        parse(retailJoin(args), retail);
        parse(join(args), ours);
        if (show(retail) != show(ours))
            ++differing;
        if (!onlyCutsDiffer(args) && ++violations <= 5)
        {
            std::string label = "sweep:";
            for (std::size_t i = 0; i < args.size(); ++i)
                label += " [" + args[i] + "]";
            check(false, label);
        }
    }
    check(violations == 0, "sweep: no parse difference other than a whole value");
    check(differing > 0, "sweep: some inputs exercise a whole value");
    std::printf("sweep %u inputs, %u with a whole value where retail cuts, %u violations\n", swept,
                differing, violations);

    std::printf("command line join %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

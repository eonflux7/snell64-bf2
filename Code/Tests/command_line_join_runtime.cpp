// [TEMP] The argv join of the console entry points (CTest bf2_command_line_join_check) against the
// recovered SimpleParser. Retail fix "The Linux server keeps command-line arguments that contain
// spaces" (docs/retail-fixes.md): the joined line must parse exactly as retail's line for every
// input, except that a value which retail cuts at its first whitespace arrives whole.
//
// retailJoin models the Linux ia-32 server main (ELF 0x8052C62 to 0x8052CA6): argv[1..] streamed
// with one ' ' between arguments and no quoting.
//
// On macOS the check also runs the native clients' relaunch round trip: BF2Engine::restartBF2
// hands the stored line to macos::relaunchExecutable, which splits it with splitCommandLine, and
// the new process joins the pieces again. That second line must parse as the first.
//
// It also checks SimpleParser::getKey for retail fix "Option values longer than 255 characters no
// longer overrun the parser's buffer" (docs/retail-fixes.md): values of up to 255 characters give
// the result of retailGetKey, a model of the retail body, and longer values arrive whole.
#include "../BF2/Game/Main/CommandLineJoin.h"
#include "../BF2/Game/Utility/SimpleParser.h"
#if defined(__APPLE__)
#include "../BF2/Platform/macOS/MacSystem.h"
#endif

#include <cctype>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
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
// quote) that retail cut at its first whitespace, losing text that follows it. A value whose only
// whitespace is at its end must parse as on retail, which cuts that whitespace off.
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
        std::string::size_type text = cut;
        while (text < o->second.size() && isSpace(o->second[text]))
            ++text;
        if (!wholeArgument || o->second.find('"') != std::string::npos
            || text == o->second.size() || r->second != o->second.substr(0, cut))
            return false;
    }
    return true;
}

#if defined(__APPLE__)
// The relaunch: the stored line split as relaunchExecutable splits it, then joined again.
std::string relaunchLine(const std::string& line)
{
    return join(dice::hfe::macos::splitCommandLine(line));
}

bool relaunchParsesAlike(const Args& args)
{
    Keys first, second;
    const std::string line = join(args);
    const bool firstParsed = parse(line, first);
    const bool secondParsed = parse(relaunchLine(line), second);
    return firstParsed == secondParsed && show(first) == show(second);
}
#endif

typedef std::map<int, std::string> Names;

// retailGetKey models the retail SimpleParser::getKey (Linux ia-32 server 0x814A4F0; BF2.exe
// 0x45CB80 and bf2_w32ded.exe 0x43FB70 have the same body): strncpy of the value into a 256-byte
// stack buffer, the terminator, then assign(buffer, strlen(buffer)). It is called only where the
// copy fits that buffer, the values of up to 255 characters on which retail is defined.
bool retailGetKey(const Names& names, const std::string& str, int* keyId, std::string* value)
{
    *keyId = -1;
    Names::const_iterator it = names.begin();
    for (; it != names.end(); ++it)
    {
        if (str.compare(0, it->second.length() + 1, it->second + " ") == 0)
        {
            *keyId = it->first;
            break;
        }
    }
    if (*keyId == -1)
        return false;

    char buffer[256];
    std::string::size_type start = it->second.length() + 1;
    std::string::size_type from, count;
    if (str.c_str()[start] != '"')
    {
        std::string::size_type end = start;
        if (start < str.length())
        {
            while (!isspace(str.c_str()[end]))
            {
                if (++end >= str.length())
                    break;
            }
        }
        from = start;
        count = end - start;
    }
    else
    {
        std::string::size_type endQuote = str.find('"', it->second.length() + 2);
        if (endQuote == std::string::npos)
            return false;
        from = start + 1;
        count = endQuote - start - 1;
    }
    if (count > 255)
    {
        check(false, "retailGetKey called with a copy of more than 255 characters");
        return false;
    }
    std::strncpy(buffer, str.c_str() + from, count);
    buffer[count] = 0;
    value->assign(buffer, std::strlen(buffer));
    return true;
}

void registerOptions(dice::hfe::SimpleParser& parser, Names& names)
{
    const std::pair<int, const char*> options[] = {
        std::make_pair(5, "password"), std::make_pair(66, "port"), std::make_pair(8, "config"),
        std::make_pair(4, "playerName"), std::make_pair(58, "?"), std::make_pair(70, "p"),
    };
    for (std::size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i)
    {
        parser.add(options[i].first, options[i].second, "");
        names.insert(std::make_pair(options[i].first, std::string(options[i].second)));
    }
}

std::string pattern(std::size_t length)
{
    std::string out;
    for (std::size_t i = 0; i < length; ++i)
        out += static_cast<char>('a' + i % 26);
    return out;
}

std::string number(std::size_t n)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(n));
    return text;
}

std::string idText(int id)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%d", id);
    return text;
}

// One getKey call against the expected outcome. value starts as "unset" so that a call which must
// leave it alone can be told apart.
void expectGetKey(dice::hfe::SimpleParser& parser, const std::string& str, bool result, int keyId,
                  const std::string& value, const std::string& label)
{
    int gotId = 1234;
    std::string gotValue = "unset";
    const bool gotResult = parser.getKey(str, &gotId, &gotValue);
    check(gotResult == result && gotId == keyId && gotValue == value,
          label + ": result " + (gotResult ? "true" : "false") + ", id " + idText(gotId)
              + ", value of " + number(gotValue.size()) + " characters");
}

void checkParserValues()
{
    dice::hfe::SimpleParser parser('+');
    Names names;
    registerOptions(parser, names);

    // Long values, unquoted and quoted, as getKey sees them from parseString (the text after the
    // '+', with parseString's trailing space) and through parseString with an option after them.
    const std::size_t lengths[] = { 255, 256, 1000, 4096 };
    for (std::size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
    {
        const std::string value = pattern(lengths[i]);
        const std::string n = number(lengths[i]);
        expectGetKey(parser, "password " + value + " ", true, 5, value, n + " characters unquoted");
        expectGetKey(parser, "password \"" + value + "\" ", true, 5, value,
                     n + " characters quoted");
        Keys keys;
        check(parser.parseString("+password " + value + " +port 16568", keys)
                  && show(keys) == "5=[" + value + "] 66=[16568]",
              n + " characters unquoted, then +port: " + number(keys.size()) + " keys");
        check(parser.parseString("+password \"" + value + "\" +port 16568", keys)
                  && show(keys) == "5=[" + value + "] 66=[16568]",
              n + " characters quoted, then +port: " + number(keys.size()) + " keys");

        // Retail's strlen ends the value at an embedded NUL; the fix keeps that for long values.
        std::string withNul = value;
        withNul[withNul.size() - 10] = '\0';
        const std::string cut = value.substr(0, value.size() - 10);
        expectGetKey(parser, std::string("password ") + withNul + " ", true, 5, cut,
                     n + " characters unquoted with a NUL 10 from the end");
        expectGetKey(parser, std::string("password \"") + withNul + "\" ", true, 5, cut,
                     n + " characters quoted with a NUL 10 from the end");

        // The failure path: an unterminated quote returns false, keeps the found id and leaves
        // the value alone.
        expectGetKey(parser, "password \"" + value + " ", false, 5, "unset",
                     n + " characters with an unterminated quote");
    }
    expectGetKey(parser, "passwordx 1 ", false, -1, "unset", "no option matches");
    expectGetKey(parser, "password", false, -1, "unset", "option without the following space");
    expectGetKey(parser, "password \"", false, 5, "unset", "only an opening quote");

    // Every length from 0 to 255 in both arms, then a sweep over short strings, against the model.
    unsigned compared = 0, differing = 0;
    for (std::size_t length = 0; length <= 255; ++length)
    {
        const std::string value = pattern(length);
        const std::string inputs[] = { "password " + value + " ", "password " + value,
                                       "password \"" + value + "\" ", "p \"" + value + "\"" };
        for (std::size_t k = 0; k < sizeof(inputs) / sizeof(inputs[0]); ++k)
        {
            int retailId = 1234, oursId = 1234;
            std::string retailValue = "unset", oursValue = "unset";
            const bool retailResult = retailGetKey(names, inputs[k], &retailId, &retailValue);
            const bool oursResult = parser.getKey(inputs[k], &oursId, &oursValue);
            ++compared;
            if ((retailResult != oursResult || retailId != oursId || retailValue != oursValue
                 || oursValue != value)
                && ++differing <= 5)
                check(false, "length " + number(length) + " input " + number(k)
                                 + ": differs from the retail model");
        }
    }

    const char* const heads[] = { "password ", "port ", "p ", "? ", "config", "playerName ",
                                  "pass ", "", "p" };
    const char alphabet[] = { 'a', 'b', ' ', '\t', '\n', '"', '\0', '+', '\xE9', '\r' };
    unsigned seed = 4242;
    for (int n = 0; n < 300000; ++n)
    {
        seed = seed * 1103515245u + 12345u;
        std::string str = heads[(seed >> 16) % (sizeof(heads) / sizeof(heads[0]))];
        seed = seed * 1103515245u + 12345u;
        // At most 255 characters after the option name and its space: no copy exceeds 255.
        const std::size_t tail = (seed >> 16) % 4 == 0 ? (seed >> 8) % 256 : (seed >> 16) % 12;
        for (std::size_t c = 0; c < tail; ++c)
        {
            seed = seed * 1103515245u + 12345u;
            const unsigned pick = (seed >> 16) % 16;
            str += pick < sizeof(alphabet) ? alphabet[pick] : 'a';
        }
        int retailId = 1234, oursId = 1234;
        std::string retailValue = "unset", oursValue = "unset";
        const bool retailResult = retailGetKey(names, str, &retailId, &retailValue);
        const bool oursResult = parser.getKey(str, &oursId, &oursValue);
        ++compared;
        if ((retailResult != oursResult || retailId != oursId || retailValue != oursValue)
            && ++differing <= 5)
            check(false, "sweep input of " + number(str.size()) + " characters differs from the "
                             "retail model");
    }
    check(differing == 0, "getKey: every value of up to 255 characters as in the retail model");
    std::printf("getKey %u inputs compared with the retail model, %u differing\n", compared,
                differing);
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
        { "empty value", A("+password", "", "+port", "16568"), "+password \"\" +port 16568",
          false },
        { "empty value at the end", A("+password", ""), "+password \"\"", false },
        { "empty first argument", A("", "+port", "16568"), "\"\" +port 16568", false },
        { "only an empty argument", A(""), "", false },
        { "blank value", A("+password", " \t", "+playerName", "x"),
          "+password \"\" +playerName x", false },
        { "only a blank argument", A(" "), " ", false },
        { "empty value after a quote", A("+playerName", "a\"b", "+password", "", "+port", "1"),
          "+playerName a\"b +password  +port 1", false },
        { "trailing spaces", A("+config", "sv.con ", "+playerName", "Bob  "),
          "+config sv.con  +playerName Bob  ", false },
        { "trailing CR", A("+config", "/srv/bf2/sv.con\r", "+port", "16567"),
          "+config /srv/bf2/sv.con\r +port 16567", false },
        { "trailing newline", A("+playerName", "Bob\n"), "+playerName Bob\n", false },
        { "leading space", A("+config", " sv.con"), "+config \" sv.con\"", true },
        { "spaced value with a trailing space", A("+config", "/srv/My Server/sv.con "),
          "+config \"/srv/My Server/sv.con \"", true },
        { "unknown option, spaced value", A("+confg", "a b"), "+confg \"a b\"", false },
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
        checkRow(rows[i]);
    check(join(Args()).empty(), "no arguments: empty line");

    // Deterministic sweep over short arguments built from option names, whitespace, quotes and
    // plus signs: no difference from retail's parse other than whole values where retail cuts.
    const char* const pieces[] = { "+config", "+port", "+?", "+", "config", "port", "x", " ",
                                   "\t", "\"", "C++", "-", "", "+port ", " +config", "\r" };
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

#if defined(__APPLE__)
    // The relaunch round trip, for every table row without a quote character (the split reads
    // quotes and backslashes as CommandLineToArgvW does, which a caller's own quoting can change)
    // and for a sweep over the same pieces without the quote.
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i)
    {
        bool quoted = false;
        for (std::size_t a = 0; a < rows[i].args.size(); ++a)
            quoted = quoted || rows[i].args[a].find('"') != std::string::npos;
        if (!quoted)
            check(relaunchParsesAlike(rows[i].args),
                  std::string(rows[i].label) + ": relaunch line ["
                      + relaunchLine(join(rows[i].args)) + "]");
    }
    unsigned relaunchSwept = 0, relaunchViolations = 0;
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
                const char* piece = pieces[(seed >> 16) % pieceCount];
                if (piece[0] != '"')
                    args[i] += piece;
            }
        }
        // An argument that holds an option and whitespace is joined unchanged, and the split
        // collapses its whitespace runs; the guide lists that limit. Such inputs are skipped.
        bool optionsWithSpace = false;
        for (std::size_t i = 0; i < args.size(); ++i)
            optionsWithSpace = optionsWithSpace
                               || (dice::hfe::commandLine::containsOptionToken(args[i])
                                   && args[i].find_first_of(" \t\n\v\f\r") != std::string::npos);
        if (optionsWithSpace)
            continue;
        ++relaunchSwept;
        if (!relaunchParsesAlike(args) && ++relaunchViolations <= 5)
        {
            std::string label = "relaunch sweep:";
            for (std::size_t i = 0; i < args.size(); ++i)
                label += " [" + args[i] + "]";
            check(false, label);
        }
    }
    check(relaunchViolations == 0, "relaunch sweep: the relaunched line parses as the first");
    std::printf("relaunch sweep %u inputs, %u violations\n", relaunchSwept, relaunchViolations);
#endif

    checkParserValues();

    std::printf("command line join %u/%u checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

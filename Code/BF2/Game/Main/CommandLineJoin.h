// Joins argv into the single command-line string that BF2::init parses, for the entry points that
// receive an argv array instead of WinMain's lpCmdLine.
//
// [CHOSEN, not recovered] Retail fix "The Linux server keeps command-line arguments that contain
// spaces" (docs/retail-fixes.md). The retail Linux main joins argv[1..] with single spaces and no
// quoting, and SimpleParser reads an unquoted value only up to the next whitespace, so a value that
// the caller passed as one argument is cut at its first space. This join quotes such a value and
// otherwise produces retail's line, so every spelling that works on retail parses as it does there.
// The same join serves the native clients and the rebuilt Windows console entry, which have no
// retail counterpart.

#ifndef GAME_MAIN_COMMANDLINEJOIN_H
#define GAME_MAIN_COMMANDLINEJOIN_H

#include <string>

namespace dice {
namespace hfe {
namespace commandLine {

// The characters SimpleParser treats as the end of an unquoted value (isspace in the C locale).
inline bool isParserSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

// True when arg contains an option token: '+' followed by at least one character other than
// whitespace, '"' and '+', and then whitespace or the end of the argument. Every registered option
// name, followed by the space SimpleParser requires, forms such a token, so an argument without one
// cannot start an option of its own.
inline bool containsOptionToken(const std::string& arg)
{
    for (std::string::size_type plus = arg.find('+'); plus != std::string::npos;
         plus = arg.find('+', plus + 1))
    {
        std::string::size_type end = plus + 1;
        while (end < arg.size() && !isParserSpace(arg[end]) && arg[end] != '"'
               && arg[end] != '+')
            ++end;
        if (end > plus + 1 && (end == arg.size() || isParserSpace(arg[end])))
            return true;
    }
    return false;
}

// Joins argv[1..argc-1] with single spaces, as the retail Linux main does. An argument is wrapped
// in quotes only when all of the following hold; retail would cut such an argument where a value
// starts at it, and the quotes change nothing else in the parse:
// - it contains whitespace;
// - it contains no quote character, and no earlier argument did (the caller then uses retail's own
//   quoting, and an added quote could close a value that one of those quotes opened);
// - it contains no option token, so it is a value and not one or more options of its own.
// Every other argument, the empty one included, is copied unchanged, as retail does.
inline std::string joinArgs(int argc, const char* const* argv)
{
    std::string out;
    bool quoteSeen = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg(argv[i] ? argv[i] : "");
        if (i > 1)
            out += ' ';

        bool hasSpace = false;
        for (std::string::size_type c = 0; c < arg.size(); ++c)
            hasSpace = hasSpace || isParserSpace(arg[c]);
        quoteSeen = quoteSeen || arg.find('"') != std::string::npos;

        if (hasSpace && !quoteSeen && !containsOptionToken(arg))
        {
            out += '"';
            out += arg;
            out += '"';
        }
        else
        {
            out += arg;
        }
    }
    return out;
}

} // namespace commandLine
} // namespace hfe
} // namespace dice

#endif // GAME_MAIN_COMMANDLINEJOIN_H

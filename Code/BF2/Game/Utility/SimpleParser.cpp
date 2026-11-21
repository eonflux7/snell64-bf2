// Parses registered numeric options into name/value pairs. [INFERRED] File placement.

#include "SimpleParser.h"

#include <cstring>      // memchr
#include <cctype>       // isspace
#include <iostream>     // std::cout - the literal 0x8990720 in showHelp is _ZSt4cout, attested by
                        // the ELF symtab entry at 0x8048FAC (value 0x8990720, size 0x8C)
#include <sstream>      // std::stringstream - basic_stringstream vtables installed in showHelp

namespace dice {
namespace hfe {

namespace {

// Assigns the count characters of str that start at begin, ended at the first NUL among them: the
// value that retail's strncpy into a stack buffer, its terminator and assign(buffer, strlen(buffer))
// produce, without the buffer and its limit (retail fix in getKey).
void assignValue(std::string* value, const std::string& str, std::string::size_type begin,
                 std::string::size_type count)
{
    const char* text = str.c_str() + begin;
    const char* nul = static_cast<const char*>(std::memchr(text, 0, count));
    value->assign(text, nul ? static_cast<std::string::size_type>(nul - text) : count);
}

} // namespace

// [INFERRED] Creates empty option maps with the requested prefix character.
SimpleParser::SimpleParser(char optionPrefix)
    : myOptionPrefix(optionPrefix)
{
}

//  (436 bytes)
void SimpleParser::add(int id, const std::string& optionName, const std::string& helpText)
{
    myOptionNames.insert(std::make_pair(id, optionName));  // : insert_unique on +0x00
    myHelpTexts.insert(std::make_pair(id, helpText));      // : insert_unique on +0x0C
    // insert_unique: duplicate ids are silently ignored, never overwritten.
}

//  (55 bytes). Return [CHOSEN] void. Never called in the shipped binary.
void SimpleParser::remove(int id)
{
    myOptionNames.erase(id);   // key passed by address = map::erase(const key&)
    myHelpTexts.erase(id);
}

//  (567 bytes). Return [CHOSEN] bool (consumed via al at 0x814A837).
bool SimpleParser::getKey(const std::string& str, int* keyId, std::string* value)
{
    *keyId = -1;                                              // unconditional, first

    // Ascending-id sweep of the name map (+0x00); first match wins.       0x814A518-0x814A5D3
    std::map<int, std::string>::iterator it = myOptionNames.begin();
    for ( ; it != myOptionNames.end(); ++it )
    {
        std::string name = it->second;
        name += " ";
        // : count re-read from the node string's length + 1 == name.length(). The option
        // must be followed by a space - parseString's appended trailing " " makes end-of-line work.
        if (str.compare(0, it->second.length() + 1, name) == 0)
        {
            *keyId = it->first;
            break;
        }
    }
    if (*keyId == -1)
        return false;

    // [CHOSEN, not recovered] Retail fix "Option values longer than 255 characters no longer
    // overrun the parser's buffer" (docs/retail-fixes.md). Retail copies the value with an
    // unclamped strncpy into char[256] at ebp-0x128 (0x814A62B, 0x814A6D0; the Windows images
    // alike) and writes the terminator after it; assignValue takes the value from str instead.
    std::string::size_type start = it->second.length() + 1;   // native string index
    if (str.c_str()[start] != '"')                            // : cmp ...,22h
    {
        std::string::size_type end = start;
        if (start < str.length())
        {
            while (!isspace(str.c_str()[end]))                // : movsx -> signed char
            {
                if (++end >= str.length())
                    break;
            }
        }
        assignValue(value, str, start, end - start);
        return true;
    }

    // Quoted value: text between the quotes. Search starts at start+1 (0x814A685: len+2).
    std::string::size_type endQuote = str.find('"', it->second.length() + 2);
    if (endQuote == std::string::npos)
        return false;   // note: *keyId keeps the found id on this failure path. Preserved.
    assignValue(value, str, start + 1, endQuote - start - 1);
    return true;
}

//  (718 bytes). Return [CHOSEN] bool (caller tests al at 0x81333BA).
bool SimpleParser::parseString(const std::string& str, std::list<std::pair<int, std::string> >& keyList)
{
    keyList.clear();
    int numFound = 0;
    int keyId;
    std::string value;                                        // reused across iterations
    std::string work(str);
    work += " ";
    std::string::size_type pos = 0;
    for (;;)
    {
        std::string::size_type hit = work.find(myOptionPrefix, pos);   // : byte [this+0x18]
        if (hit == std::string::npos)
            break;
        pos = hit + 1;   // : scanning resumes right after the prefix char - a prefix char
                         // inside a quoted value is visited again on the next iteration. Preserved.
        if (getKey(work.substr(pos), &keyId, &value))         //  substr / 0x814A832 call
            keyList.insert(keyList.end(), std::make_pair(keyId, value));  //  == push_back
        ++numFound;      // : counted for every prefix hit, even failed getKey()s
    }
    return numFound != 0;
}

//  (1560 bytes)
void SimpleParser::showHelp(const std::string& appName)
{
    std::stringstream help;   // inlined basic_stringstream ctor, mode in|out (0x814AA1C-0x814ABF0);
                              // proven stringstream, not ostringstream (istream base initialised too)

    help << "Usage: " << appName << " <options>" << std::endl;
    help << std::endl;
    help << "Available options are:" << std::endl;

    {
        std::map<int, std::string>::iterator name = myOptionNames.begin();   // : +0x00
        std::map<int, std::string>::iterator text = myHelpTexts.begin();     // : +0x0C
        // Both conditions checked each round (0x814AC9C, 0x814ACA7) - stops at the shorter map.
        for ( ; name != myOptionNames.end() && text != myHelpTexts.end(); ++name, ++text )
        {
            if (!text->second.empty())
            {
                // prefix printed as a char (0x814AD0A movsx [this+0x18] -> operator<<(ostream&,char));
                // separator is "  -  ", two spaces each side (0x814AD29, literal 0x8893850).
                help << " " << myOptionPrefix << name->second
                     << "  -  " << text->second << std::endl;
            }
        }
    }

    help << std::endl;
    help << "Advanced options:" << std::endl;

    {
        std::map<int, std::string>::iterator name = myOptionNames.begin();
        std::map<int, std::string>::iterator text = myHelpTexts.begin();
        for ( ; name != myOptionNames.end() && text != myHelpTexts.end(); ++name, ++text )
        {
            if (text->second.empty())   // advanced == registered with an empty help text
                help << " " << myOptionPrefix << name->second << std::endl;
        }
    }

    std::cout << help.str();   // : 0x8990720 == _ZSt4cout (ELF symtab). No flush.
}

} // namespace hfe
} // namespace dice

// [CHOSEN, not recovered] macOS port: a game that restarts itself keeps its place in front.
//
// BF2Engine::restartBF2 starts a new instance of the client (relaunchExecutable, MacSystem.cpp;
// retail calls ShellExecuteA with SW_SHOWNORMAL) and lets the running one quit; a player
// triggers it by activating a mod. On macOS the new instance has to ask for activation, and the
// Cocoa host repeats that request only against the application that started the client
// (CocoaHost.mm, beginActivation). A restarted instance has no such application: once the first
// instance has exited its parent is launchd, and whatever application the system puts in front
// in the first instance's place is unrelated to it.
//
// So the first instance says whether it was the active application when it restarted, and only
// then is the new instance entitled to the front. It says so with an environment variable that
// it sets for that one child. The variable is set or removed at every restart, never inherited:
// a later generation, restarted while the game was in the background, does not carry it.
//
// Plain C++ without AppKit: MacSystem.cpp builds the child's environment with it, the Cocoa host
// reads the variable, and a headless check pins both.
#ifndef PLATFORM_MACOS_RESTARTINFRONT_H
#define PLATFORM_MACOS_RESTARTINFRONT_H

#include <cstring>
#include <string>
#include <vector>

namespace dice { namespace hfe { namespace macos {

// The variable. The value "1" says: the instance that started this process through
// relaunchExecutable was the active application at that moment. Any other value, and no variable,
// say nothing.
const char* const kRestartedInFrontVariable = "BF2_RESTARTED_IN_FRONT";

// Whether `value`, the variable's value or null when it is not set, marks such a start.
inline bool restartedInFront(const char* value)
{
    return value && value[0] == '1' && value[1] == '\0';
}

// The environment for the instance a restart starts: every entry of `inherited` (an environ
// array: "NAME=value" strings, ended by a null pointer) except the variable, in their order, and
// after them the variable with the value "1" when `inFront`. An inherited entry of the variable
// is dropped in both cases: it described how this process was started, not how it restarts.
inline std::vector<std::string> restartEnvironment(const char* const* inherited, bool inFront)
{
    const std::string prefix = std::string(kRestartedInFrontVariable) + "=";
    std::vector<std::string> environment;
    for (; inherited && *inherited; ++inherited)
        if (std::strncmp(*inherited, prefix.c_str(), prefix.size()) != 0)
            environment.push_back(*inherited);
    if (inFront)
        environment.push_back(prefix + "1");
    return environment;
}

// Whether this process holds the front now, so that an instance it starts may take its place:
// the Cocoa host reports the application active (for AppKit, or for the system while the host
// uses the system's state), and that focus is real. A run whose focus an input script forces
// ([TEMP] BF2_INPUT_SCRIPT without BF2_INPUT_SCRIPT_REAL_FOCUS) reports focus whether or not it
// is in front, and hands nothing on. Defined in CocoaHost.mm.
bool clientHoldsFront();

} } }
#endif

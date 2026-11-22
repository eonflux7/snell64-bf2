// [TEMP] Content-free checks of the anti-aliasing apply rule (RendMisc/AntialiasingPolicy.h),
// issue #60. No renderer, profile or settings repository is modelled.
#include "../BF2/RendMisc/AntialiasingPolicy.h"
#include <cstdio>

namespace ap = dice::hfe::aapolicy;
namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
}

int main()
{
    check(ap::pushesAntialiasing(false), "no game loaded: the level is pushed");
    check(!ap::pushesAntialiasing(true), "a game is loaded: nothing is pushed");

    check(!ap::antialiasingNeedsRestart(false, true), "menu with a renderer: applied at once, no restart");
    check(ap::antialiasingNeedsRestart(true, true), "game loaded: restart needed");
    check(ap::antialiasingNeedsRestart(false, false), "no renderer: restart needed");
    check(ap::antialiasingNeedsRestart(true, false), "game loaded and no renderer: restart needed");

    check(ap::defineInputsChanged(false, true), "off to on: effects are stale");
    check(ap::defineInputsChanged(true, false), "on to off: effects are stale");
    check(!ap::defineInputsChanged(true, true), "on to on (4x to 8x): nothing to reload");
    check(!ap::defineInputsChanged(false, false), "off to off: nothing to reload");

    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

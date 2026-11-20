// [TEMP] Content-free checks of the native hosts' display-mode policy (RendMisc/DisplayModePolicy.h):
// the fullscreen start mode and the new-profile resolution. The mode lists are those the hosts
// report (pixel sizes; the macOS desktop in points). No device, renderer or profile is modelled.
#include "../BF2/RendMisc/DisplayModePolicy.h"
#include "../BF2/Menu/MenuViewport.h"
#include <cstdio>
#include <vector>

namespace dm = dice::hfe::displaymode;
namespace {
unsigned checks = 0, failures = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
int pick(const std::vector<dm::Mode>& modes, dm::Mode desktop, unsigned minWidth = 800)
{
    return modes.empty() ? -1 : dm::preferredMode(&modes[0], modes.size(), desktop, minWidth);
}
bool is(const std::vector<dm::Mode>& modes, int index, unsigned w, unsigned h, unsigned hz)
{
    return index >= 0 && modes[index].width == w && modes[index].height == h && modes[index].refresh == hz;
}
}

int main()
{
    // Intel MacBook Pro 13" (2560x1600 panel, desktop 1440x900 points): the list repeats the point
    // sizes as low-resolution modes, so the largest fitting mode is the point size itself.
    const std::vector<dm::Mode> intel = {
        {2560, 1600, 60}, {3360, 2100, 60}, {2880, 1800, 60}, {2048, 1280, 60}, {1650, 1050, 60},
        {1440, 900, 60}, {1280, 800, 60}, {1152, 720, 60}, {1024, 768, 60}, {800, 600, 60}, {640, 480, 60}};
    check(is(intel, pick(intel, {1440, 900, 60}), 1440, 900, 60), "Intel Retina: desktop point size");
    check(dm::listsSize(&intel[0], intel.size(), 800, 600), "Intel lists 800x600: the start keeps it");

    // MacBook Air M2 (2560x1664 panel, desktop 1470x956 points): only backing sizes are listed.
    const std::vector<dm::Mode> m2 = {
        {2048, 1330, 60}, {2560, 1664, 60}, {2940, 1912, 60}, {3420, 2224, 60}};
    check(!dm::listsSize(&m2[0], m2.size(), 800, 600), "M2 has no 800x600");
    check(is(m2, pick(m2, {1470, 956, 60}), 2940, 1912, 60), "M2: the 2x backing store of the desktop");
    check(is(m2, pick(m2, {1280, 832, 60}), 2560, 1664, 60), "M2 at 'larger text': 2x of 1280x832");
    // A desktop with no whole multiple listed: the smallest larger mode.
    check(is(m2, pick(m2, {1600, 1000, 60}), 2048, 1330, 60), "no multiple: smallest larger mode");
    // A 3x multiple counts when 2x is missing.
    const std::vector<dm::Mode> notMultiple = {{3000, 2000, 60}, {2400, 1500, 60}};
    check(is(notMultiple, pick(notMultiple, {1000, 600, 60}), 2400, 1500, 60),
          "3000x2000 is no multiple of 1000x600: smallest larger mode");
    const std::vector<dm::Mode> triple2 = {{3000, 1800, 60}, {2600, 1600, 60}};
    check(is(triple2, pick(triple2, {1000, 600, 60}), 3000, 1800, 60), "3x backing when 2x is absent");

    // Refresh ties: the desktop's rate first, then the highest.
    const std::vector<dm::Mode> rates = {{1920, 1080, 60}, {1920, 1080, 144}, {1920, 1080, 120}};
    check(is(rates, pick(rates, {1920, 1080, 120}), 1920, 1080, 120), "desktop refresh wins a tie");
    check(is(rates, pick(rates, {1920, 1080, 75}), 1920, 1080, 144), "otherwise the highest refresh");
    const std::vector<dm::Mode> pro = {{3024, 1964, 60}, {3024, 1964, 120}, {2048, 1330, 120}};
    check(is(pro, pick(pro, {1512, 982, 120}), 3024, 1964, 120), "ProMotion backing at 120 Hz");

    // Windows/Linux: pixel desktop, the desktop's own mode fits.
    const std::vector<dm::Mode> deck = {{1280, 800, 60}, {800, 600, 60}, {640, 400, 60}};
    check(is(deck, pick(deck, {1280, 800, 60}), 1280, 800, 60), "Deck: the panel mode");

    // The 800-pixel floor of the resolution list, and the empty cases.
    const std::vector<dm::Mode> small = {{640, 480, 60}, {720, 450, 60}};
    check(pick(small, {1440, 900, 60}) < 0, "nothing at least 800 wide");
    check(pick(small, {1440, 900, 60}, 0) == 1, "without the floor the largest fitting");
    check(pick(m2, {0, 0, 0}) < 0, "unknown desktop");
    check(pick(std::vector<dm::Mode>(), {1440, 900, 60}) < 0, "empty list");

    // The HiDPI UI factor (console, cursor): the menu viewport height over 600, nearest whole.
    using dice::hfe::ui::MenuViewport;
    check(MenuViewport::uiScale(800, 600) == 1, "800x600: 1");
    check(MenuViewport::uiScale(1024, 768) == 1, "1024x768: 1");
    check(MenuViewport::uiScale(1280, 800) == 1, "1280x800: 1");
    check(MenuViewport::uiScale(1440, 900) == 1, "1440x900: 1 (1.5 rounds down)");
    check(MenuViewport::uiScale(1920, 1080) == 2, "1920x1080: 2");
    check(MenuViewport::uiScale(2560, 1600) == 3, "2560x1600: 3");
    check(MenuViewport::uiScale(2560, 1664) == 3, "2560x1664: 3");
    check(MenuViewport::uiScale(2940, 1912) == 3, "2940x1912: 3");
    check(MenuViewport::uiScale(3840, 2160) == 4, "3840x2160: 4");
    check(MenuViewport::uiScale(1600, 900) == 1, "16:9 1600x900: 1");
    check(MenuViewport::uiScale(2560, 1080) == 2, "21:9 2560x1080: height decides, 2");
    check(MenuViewport::uiScale(1200, 1600) == 1, "portrait 1200x1600: 4:3 width-limited (900), 1");
    check(MenuViewport::uiScale(0, 0) == 1, "degenerate: 1");
    // The cursor factor rounds down: never larger against the UI than 32 pixels on 600 lines.
    check(MenuViewport::cursorScale(1440, 900) == 1, "cursor 1440x900: 1");
    check(MenuViewport::cursorScale(1920, 1080) == 1, "cursor 1920x1080: 1");
    check(MenuViewport::cursorScale(1920, 1200) == 2, "cursor 1920x1200: 2");
    check(MenuViewport::cursorScale(2560, 1600) == 2, "cursor 2560x1600: 2");
    check(MenuViewport::cursorScale(2560, 1664) == 2, "cursor 2560x1664: 2");
    check(MenuViewport::cursorScale(2940, 1912) == 3, "cursor 2940x1912: 3");
    check(MenuViewport::cursorScale(3840, 2160) == 3, "cursor 3840x2160: 3");
    check(MenuViewport::cursorScale(0, 0) == 1, "cursor degenerate: 1");

    std::printf("display_mode_policy: %u/%u passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

// [TEMP] [CHOSEN, not recovered] Linux fullscreen display-mode contract, issue #36.
// A fullscreen client resize applies the matching display mode, so DXVK's swapchain (sized from
// the window's pixels) follows the back buffer. SDL mode calls are observed below; the dummy
// driver never changes a real display.
// [RETIRE-WHEN: linux-host-contract-suite-replaced] Keep until equivalent native host coverage exists.
#include "../../BF2/Platform/Host/ClientHost.h"
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
struct Size { int w, h; bool operator==(const Size& o) const { return w == o.w && h == o.h; } };
std::vector<Size> applied;
SDL_DisplayMode current{};
bool haveCurrent = false;
void check(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s (%s)\n", message, SDL_GetError()); std::exit(1); }
}
}

extern "C" {
SDL_Window* __real_SDL_CreateWindow(const char*, int, int, SDL_WindowFlags);
// The dummy driver has no Vulkan; the host's window flags are otherwise kept.
SDL_Window* __wrap_SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags)
{
    return __real_SDL_CreateWindow(title, w, h, flags & ~SDL_WINDOW_VULKAN);
}
// The display lists every requested size at 90 Hz, as the Deck panel lists its emulated modes.
bool __wrap_SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID display, int w, int h, float, bool, SDL_DisplayMode* mode)
{
    *mode = SDL_DisplayMode{};
    mode->displayID = display;
    mode->format = SDL_PIXELFORMAT_XRGB8888;
    mode->w = w;
    mode->h = h;
    mode->pixel_density = 1.0f;
    mode->refresh_rate = 90.0f;
    return true;
}
bool __wrap_SDL_SetWindowFullscreenMode(SDL_Window*, const SDL_DisplayMode* mode)
{
    check(mode != nullptr, "host asks for an explicit mode");
    applied.push_back({mode->w, mode->h});
    current = *mode;
    haveCurrent = true;
    return true;
}
const SDL_DisplayMode* __wrap_SDL_GetWindowFullscreenMode(SDL_Window*)
{
    return haveCurrent ? &current : nullptr;
}
}

int main()
{
    using namespace dice::hfe::host;
    ClientHost& host = ClientHost::instance();
    check(host.initApplication(), "initialize headless SDL");
    HostWindowDesc desc{};
    desc.title = "fullscreen check";
    desc.width = 800;
    desc.height = 600;
    desc.fullscreen = true;
    check(host.createWindow(desc), "create fullscreen window");
    check(applied == std::vector<Size>({{800, 600}}), "fullscreen start applies the start mode");
    host.resizeClient(1280, 800);
    check(applied == std::vector<Size>({{800, 600}, {1280, 800}}), "fullscreen resize applies the new mode");
    check(host.clientWidth() == 1280 && host.clientHeight() == 800, "client size follows the mode");
    host.resizeClient(1280, 800);
    check(applied.size() == 2, "unchanged mode is not reapplied");
    host.setFullscreen(false);
    check(!host.isFullscreen(), "leave fullscreen");
    host.resizeClient(1024, 768);
    check(applied.size() == 2, "windowed resize sets no display mode");
    host.setFullscreen(true);
    check(host.isFullscreen(), "enter fullscreen");
    check(applied.back() == Size({1024, 768}), "entering fullscreen applies the client size's mode");
    host.destroyWindow();
    std::puts("PASS: Linux fullscreen display mode follows the client size");
    return 0;
}

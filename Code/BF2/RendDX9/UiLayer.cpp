// [UI] The fork's in-game UI on Dear ImGui (docs/lanes/ui.md). Not part of retail BF2.
//
// One ImGui context, created at the first Present. The menu key (renderer.uiMenuKey, Insert by
// default) opens and closes the menu; the panels are in UiDebug.cpp and UiConsole.cpp (UiPanels.h).
// The console window shows while the game's console is active, and types instead of the retail
// console. The window procedure is subclassed so ImGui sees the
// window's messages; while the menu or the console is shown, mouse, key and character messages go
// to ImGui only, and BF2.exe (through IRendererUi) releases the DirectInput mouse; while the menu
// is open it delivers no game input either.
// The UI is drawn onto the back buffer just before Present, after the frame and its screenshot,
// with its own BeginScene/EndScene; render target, depth surface and the sRGB states are put back
// afterwards (the DX9 backend's state block covers the rest), so StateManager's caches stay true.
#ifdef BF2_IMAGE_RENDDX9

#include <windows.h>

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "UiLayer.h"
#include "UiPanels.h"
#include "../RendMisc/IRendererUi.h"

#include "../External/imgui/imgui.h"
#include "../External/imgui/backends/imgui_impl_dx9.h"
#include "../External/imgui/backends/imgui_impl_win32.h"

#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam,
                                                             LPARAM lParam);

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

ImGuiContext* g_context = 0;
IDirect3DDevice9* g_device = 0;   // the device the DX9 backend was initialised with
HWND g_window = 0;
WNDPROC g_gameWndProc = 0;        // the window procedure we chained in front of
int g_menuKey = VK_INSERT;
volatile bool g_menuOpen = false; // written by the window procedure, read by the game thread
bool g_consoleShown = false;      // as of the last frame drawn

RendDX9Base* base()
{
    return static_cast<RendDX9Base*>(g_rendererBase);
}

bool isInputMessage(UINT message)
{
    return (message >= WM_KEYFIRST && message <= WM_KEYLAST && message != WM_SYSKEYDOWN
            && message != WM_SYSKEYUP && message != WM_SYSCHAR)
        || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST);
}

void clearInput()
{
    if (!g_context)
        return;
    ImGuiIO& io = ImGui::GetIO();
    io.ClearInputKeys();
    io.ClearInputMouse();
}

void setMenuOpen(bool open)
{
    if (open == g_menuOpen)
        return;
    g_menuOpen = open;
    clearInput();
}

// The menu or the console window has the keyboard and mouse.
bool uiHasInput()
{
    return g_context && (g_menuOpen || uiConsoleShown());
}

// The character the console key types (scan code 0x29, under Esc, on every layout; 0xA7 is the
// retail console's own toggle character): the console window never sees it.
bool isConsoleKeyCharacter(UINT message, WPARAM wParam, LPARAM lParam)
{
    return message == WM_CHAR && (((lParam >> 16) & 0xFF) == 0x29 || wParam == 0xA7);
}

LRESULT CALLBACK uiWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    const bool firstPress = (lParam & (1L << 30)) == 0;
    if (message == WM_KEYDOWN && static_cast<int>(wParam) == g_menuKey && firstPress)
    {
        setMenuOpen(!g_menuOpen);
        return 0;
    }
    if (message == WM_KEYUP && static_cast<int>(wParam) == g_menuKey)
        return 0;
    if (uiHasInput())
    {
        if (!isConsoleKeyCharacter(message, wParam, lParam))
            ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam);
        if (isInputMessage(message))
            return 0;                     // the game sees none of the UI's input
    }
    return ::CallWindowProcA(g_gameWndProc, window, message, wParam, lParam);
}

// The fork's look: ImGui's dark style on a slate background, one accent (a muted teal), and
// rounded frames. Sizes here are at 1080p; applyScale scales them.
void applyTheme(ImGuiStyle& style)
{
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(12.0f, 10.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.SeparatorTextBorderSize = 2.0f;

    const ImVec4 accent(0.20f, 0.62f, 0.60f, 1.00f);
    const ImVec4 accentHover(0.26f, 0.72f, 0.70f, 1.00f);
    const ImVec4 accentDim(0.20f, 0.62f, 0.60f, 0.40f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.92f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.56f, 0.60f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.09f, 0.10f, 0.12f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.60f);
    c[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.27f, 0.30f, 0.60f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.24f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.24f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.09f, 0.10f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.11f, 0.13f, 0.15f, 1.00f);
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered] = accentDim;
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.21f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderHovered] = accentDim;
    c[ImGuiCol_HeaderActive] = accent;
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.27f, 0.30f, 0.80f);
    c[ImGuiCol_SeparatorHovered] = accentDim;
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = accentDim;
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = ImVec4(0.13f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered] = accentDim;
    c[ImGuiCol_TabSelected] = ImVec4(0.17f, 0.36f, 0.36f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = accentHover;
    c[ImGuiCol_TabDimmed] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.15f, 0.25f, 0.26f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.13f, 0.15f, 0.17f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.03f);
    c[ImGuiCol_TextSelectedBg] = accentDim;
    c[ImGuiCol_NavCursor] = accentHover;
}

void applyScale(float displayHeight)
{
    // Sized for 1080p and scaled with the display height, so the menu reads the same at any size.
    const float scale = displayHeight > 0.0f ? displayHeight / 1080.0f : 1.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    applyTheme(style);
    style.ScaleAllSizes(scale);
    // ScaleAllSizes truncates the cursor scale to a whole number: 0 below 1080p, so no cursor.
    style.MouseCursorScale = scale > 1.0f ? scale : 1.0f;
    style.FontSizeBase = 16.0f;
    style.FontScaleMain = scale;
}

bool create()
{
    RendDX9Base* const b = base();
    if (!b || !b->device)
        return false;
    HWND window = b->pp.hDeviceWindow;
    if (!window)
        return false;

    IMGUI_CHECKVERSION();
    g_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = 0;                   // nothing written next to the game
    io.LogFilename = 0;
    io.Fonts->AddFontDefaultVector();
    applyScale(static_cast<float>(b->pp.BackBufferHeight));

    if (!ImGui_ImplWin32_Init(window) || !ImGui_ImplDX9_Init(b->device))
    {
        std::fprintf(stderr, "[UI] ImGui backend init failed\n");
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_context);
        g_context = 0;
        return false;
    }
    g_device = b->device;
    if (window != g_window)               // still chained from an earlier layer: not again
    {
        g_window = window;
        g_gameWndProc = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&uiWndProc)));
    }
    std::fprintf(stderr, "[UI] ImGui %s ready; menu key 0x%02X\n", IMGUI_VERSION, g_menuKey);
    return true;
}

void render(IDirect3DSwapChain9* chain)
{
    IDirect3DDevice9* const device = g_device;
    IDirect3DSurface9* backBuffer = 0;
    if (chain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer) < 0)
        return;
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    // The cursor is ImGui's only while the menu or the console has the mouse; otherwise the system
    // cursor is left alone (the game keeps it hidden).
    const bool hasMouse = g_menuOpen || g_consoleShown;
    io.MouseDrawCursor = hasMouse;
    if (hasMouse)
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::NewFrame();
    if (uiOverlayShown())
        uiDebugDrawOverlay();
    if (g_consoleShown)
        uiConsoleDraw(g_menuOpen);
    if (g_menuOpen)
        uiDebugDrawMenu();
    ImGui::Render();

    IDirect3DSurface9* savedTarget = 0;
    IDirect3DSurface9* savedDepth = 0;
    device->GetRenderTarget(0, &savedTarget);
    device->GetDepthStencilSurface(&savedDepth);
    DWORD srgbWrite = 0;
    DWORD srgbTexture = 0;
    device->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgbWrite);
    device->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgbTexture);

    device->SetRenderTarget(0, backBuffer);
    device->SetDepthStencilSurface(0);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    if (device->BeginScene() >= 0)
    {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }

    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, srgbTexture);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
    device->SetRenderTarget(0, savedTarget);
    device->SetDepthStencilSurface(savedDepth);
    if (savedTarget)
        savedTarget->Release();
    if (savedDepth)
        savedDepth->Release();
    backBuffer->Release();
}

class RendererUi : public IRendererUi
{
public:
    virtual bool uiCapturesInput() const { return g_menuOpen; }
    virtual bool uiOwnsConsole() const { return g_context != 0; }
    virtual bool uiWantsMouse() const { return uiHasInput(); }
    virtual void uiSetHost(IUiHost* host) { uiDebugSetHost(host); }
};

RendererUi g_rendererUi;

} // namespace

void uiLayerPresent(IDirect3DSwapChain9* chain)
{
    if (!g_context && !create())
        return;
    uiDebugFrameStart();
    const bool consoleShown = uiConsoleShown();
    if (consoleShown != g_consoleShown)
    {
        g_consoleShown = consoleShown;
        clearInput();
        if (!consoleShown)
            uiConsoleHidden();
    }
    if ((g_menuOpen || g_consoleShown || uiOverlayShown()) && chain)
        render(chain);
    uiDebugBeforePresent();
}

void uiLayerPresented()
{
    if (g_context)
        uiDebugPresented();
}

void uiLayerDeviceLost()
{
    if (g_context)
        ImGui_ImplDX9_InvalidateDeviceObjects();
}

void uiLayerShutdown()
{
    if (!g_context)
        return;
    g_menuOpen = false;
    // Unchain only while we are still in front; if something chained after us, uiWndProc stays in
    // the chain and keeps forwarding (it does nothing else without a context).
    if (g_window
        && ::GetWindowLongPtrA(g_window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&uiWndProc))
    {
        ::SetWindowLongPtrA(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_gameWndProc));
        g_window = 0;
        g_gameWndProc = 0;
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_context);
    g_context = 0;
    g_device = 0;
}

IRendererUi* uiLayerInterface()
{
    return &g_rendererUi;
}

int uiLayerGetMenuKey()
{
    return g_menuKey;
}

void uiLayerSetMenuKey(int key)
{
    if (key <= 0 || key >= 256 || key == g_menuKey)
        return;
    g_menuKey = key;
    uiSaveSettings();
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

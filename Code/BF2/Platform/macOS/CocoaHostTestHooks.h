// [TEMP] [CHOSEN, not recovered] Cocoa-only host fixture access; never part of the shared host.
// [RETIRE-WHEN: macos-client-window] Replace with equivalent native host/input coverage before removal.
#ifndef PLATFORM_MACOS_COCOAHOSTTESTHOOKS_H
#define PLATFORM_MACOS_COCOAHOSTTESTHOOKS_H

#if defined(__APPLE__) && defined(OPENBF2_HOST_TEST_SEAMS)
#include "../Host/ClientHost.h"

namespace dice { namespace hfe { namespace macos {

class CocoaHostTestHooks
{
public:
    // Borrowed Cocoa objects for the window fixture; null when the host has no window.
    static void* nativeWindow(host::ClientHost& host);
    static void* nativeView(host::ClientHost& host);
    static void* metalLayer(host::ClientHost& host);
    static bool isISOKeyboard(const host::ClientHost& host);

    // Simulates focus when AppKit activation is unavailable in the test session. The focus set
    // here stands: the host stops following AppKit unless setSystemFocusForTest simulates it.
    static void overrideFocusForTest(host::ClientHost& host, bool windowKey, bool appActive);

    // What AppKit and the system would report about the application and the game window.
    struct SystemFocus
    {
        bool   appActive;       // [NSApp isActive]
        bool   windowKey;       // the game window is the key window
        bool   windowOnScreen;  // visible and not miniaturized
        bool   modalWindow;     // a modal window of this process, or a sheet on the game window
        bool   otherKeyWindow;  // another key, or visible and key-capable, window of this process
        bool   makeKeyWorks;    // making the game window key succeeds
        int    frontPid;        // the system's frontmost application; 0: none
        double now;             // the clock of the focus repair, in seconds
    };
    // Simulates that state for the host's focus repair (the per-pump reconciliation and the
    // repair on a press), which then runs without an application or a window; null ends it.
    static void setSystemFocusForTest(host::ClientHost& host, const SystemFocus* state);
    // The delegate callbacks, after the simulated state was changed to match.
    enum FocusCallback { WindowBecameKey, WindowResignedKey, AppBecameActive, AppResignedActive };
    static void focusCallbackForTest(host::ClientHost& host, FocusCallback callback);
    // The focus part of pumpEvents.
    static void pumpFocusForTest(host::ClientHost& host);
    struct FocusRepairStats
    {
        bool     windowKey;     // the simulated window's key status now
        bool     systemActive;  // the system-state fallback is in use
        int      makeKeyCalls;  // attempts to make the simulated window key
        unsigned droppedInputs; // input events the focus gate dropped
        unsigned dropReports;   // "[host] input dropped" lines written
    };
    static FocusRepairStats focusRepairStatsForTest(const host::ClientHost& host);
    // Routes injected NSEvents without creating a window or touching the hardware cursor.
    static void enableHeadlessForTest(host::ClientHost& host);
    static void injectEventForTest(host::ClientHost& host, void* nsEvent);
    static void cursorStateForTest(const host::ClientHost& host, bool& hidden, bool& disassociated);
    static void pumpRawMouseForTest(host::ClientHost& host);
};

} } }
#endif
#endif

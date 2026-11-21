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
        double now;             // the clock of the focus repair and the activation, in seconds
        // The launch activation. With `activation` the simulation covers it: the host decides as
        // it does with a window, and its requests and Dock resets are counted instead of made
        // (activationStatsForTest). Without it a simulated host never asks for activation.
        bool   activation;
        int    parentPid;       // this process's parent
        int    ancestors[6];    // the parent's own ancestors, nearest first; 0 ends the list
        bool   ancestryDenied;  // the process lookup fails, as in a sandbox that denies it
        int    dockPid;         // the Dock's pid; 0: there is no Dock process
    };
    // Simulates that state for the host's focus repair (the per-pump reconciliation and the
    // repair on a press), which then runs without an application or a window; null ends it.
    static void setSystemFocusForTest(host::ClientHost& host, const SystemFocus* state);
    // The delegate callbacks, after the simulated state was changed to match.
    enum FocusCallback { WindowBecameKey, WindowResignedKey, AppBecameActive, AppResignedActive };
    static void focusCallbackForTest(host::ClientHost& host, FocusCallback callback);
    // The focus part of pumpEvents: the launch activation, then the reconciliation. One call is
    // one look of the host at the activation period. `insideCreateWindow` makes it the look that
    // createWindow runs right after the first request: the period's ten seconds do not count
    // from that one but from the next look, as from the first frame's pump in the client.
    static void pumpFocusForTest(host::ClientHost& host, bool insideCreateWindow = false);
    // The first activation request, as createWindow makes it when the game window is shown.
    // createWindow then looks once (pumpFocusForTest with insideCreateWindow).
    static void beginActivationForTest(host::ClientHost& host);
    // A first request either ignores other applications or is cooperative, that is, it yields to
    // an active application on every macOS version. Before macOS 14 the two are
    // activateIgnoringOtherApps:YES and activateIgnoringOtherApps:NO; from macOS 14 on both are
    // -[NSApplication activate], which the system may refuse. The repeated requests all ignore
    // other applications.
    struct ActivationStats
    {
        bool pending;           // an activation period is running
        int  requests;          // first requests made, one per beginActivation
        int  ignoringOthers;    // those of them that ignore other applications
        int  retries;           // repeated requests made (every half second, and on a click)
        int  dockResets;        // times the Dock was activated to reset a pending activation
    };
    static ActivationStats activationStatsForTest(const host::ClientHost& host);
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

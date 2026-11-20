// [TEMP] macOS input review harness. Runs the Cocoa host headless
// (CocoaHostTestHooks::enableHeadlessForTest: no window, no NSApplication, the real cursor untouched),
// routes NSEvents built from CoreGraphics events through it exactly as pumpEvents routes them,
// drives NativeInputDeviceManager with InputManager::update's protocol (update(-1) polls, then one
// positive pass per input tick, 10.0 on the last) and checks what the engine-facing device
// interfaces report against the DirectInput semantics of the recovered DX8 devices
// (KeyboardDevice.cpp, MouseDevice.cpp). The host's focus repair and its launch activation run
// against a simulated AppKit state (CocoaHostTestHooks::setSystemFocusForTest): the activation's
// requests and Dock resets are counted, none is made. [RETIRE-WHEN: macos-client-window]

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>

#include "../../BF2/IO/Input/NativeInputDeviceManager.h"
#include "../../BF2/Platform/Host/ClientHost.h"
#include "../../BF2/Platform/macOS/CocoaHostTestHooks.h"
#include "../../BF2/Platform/macOS/CursorScale.h"
#include "../../BF2/Platform/macOS/MacKeyMap.h"
#include "../../BF2/Platform/macOS/MacRawMouse.h"
#include "../../BF2/Platform/macOS/ProcessAncestry.h"
#include "../native_host/WindowMessageRecorder.h"
#include "../../BF2/Core/Ids.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <atomic>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

namespace dice { namespace hfe { namespace io { void installControlMapFixture(); extern IBase* inputDeviceManager; } } }

using namespace dice::hfe;
using host::ClientHost;
using macos::CocoaHostTestHooks;

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_test;

void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("  FAIL [%s] %s\n", g_test.c_str(), what);
    }
}

void begin(const char* name) { g_test = name; std::printf("[test] %s\n", name); }

// --- event construction ----------------------------------------------------------------------

// Event times on the host clock (system uptime), as hardware events carry them.
CGEventTimestamp nowNs(int offsetMs = 0)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + static_cast<long long>(offsetMs) * 1000000LL;
}

const SInt16 kKeyboardANSI = 40;    // KBGetLayoutType(40) == 'ANSI' (Apple Extended/MacBook ANSI)
const SInt16 kKeyboardISO  = 41;    // KBGetLayoutType(41) == 'ISO '

void inject(CGEventRef cg)
{
    NSEvent* e = [NSEvent eventWithCGEvent:cg];
    CFRelease(cg);
    CocoaHostTestHooks::injectEventForTest(ClientHost::instance(), (__bridge void*)e);
}

void key(unsigned short keyCode, bool down, bool repeat = false, SInt16 kbType = kKeyboardANSI,
         CGEventFlags flags = 0, int offsetMs = 0)
{
    CGEventRef cg = CGEventCreateKeyboardEvent(0, keyCode, down);
    CGEventSetFlags(cg, flags);
    CGEventSetIntegerValueField(cg, kCGKeyboardEventKeyboardType, kbType);
    if (repeat) CGEventSetIntegerValueField(cg, kCGKeyboardEventAutorepeat, 1);
    CGEventSetTimestamp(cg, nowNs(offsetMs));
    inject(cg);
}

// flagsChanged: the modifier key's code with the flag set after the change.
void flags(unsigned short keyCode, CGEventFlags after)
{
    CGEventRef cg = CGEventCreateKeyboardEvent(0, keyCode, true);
    CGEventSetType(cg, kCGEventFlagsChanged);
    CGEventSetFlags(cg, after);
    CGEventSetIntegerValueField(cg, kCGKeyboardEventKeyboardType, kKeyboardANSI);
    CGEventSetTimestamp(cg, nowNs());
    inject(cg);
}

void move(int dx, int dy)
{
    CGEventRef cg = CGEventCreateMouseEvent(0, kCGEventMouseMoved, CGPointMake(100, 100), kCGMouseButtonLeft);
    CGEventSetIntegerValueField(cg, kCGMouseEventDeltaX, dx);
    CGEventSetIntegerValueField(cg, kCGMouseEventDeltaY, dy);
    CGEventSetTimestamp(cg, nowNs());
    inject(cg);
}

void button(int number, bool down, int offsetMs = 0)
{
    CGEventType type;
    if (number == 0)      type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
    else if (number == 1) type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
    else                  type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
    CGEventRef cg = CGEventCreateMouseEvent(0, type, CGPointMake(100, 100), static_cast<CGMouseButton>(number));
    CGEventSetIntegerValueField(cg, kCGMouseEventButtonNumber, number);
    CGEventSetTimestamp(cg, nowNs(offsetMs));
    inject(cg);
}

// A wheel event; returns the sign AppKit gives the device direction (natural scrolling undone).
void wheelLines(int lines)
{
    CGEventRef cg = CGEventCreateScrollWheelEvent2(0, kCGScrollEventUnitLine, 1, lines, 0, 0);
    CGEventSetTimestamp(cg, nowNs());
    inject(cg);
}

void wheelPixels(int pixels, CGMomentumScrollPhase momentum = kCGMomentumScrollPhaseNone)
{
    CGEventRef cg = CGEventCreateScrollWheelEvent2(0, kCGScrollEventUnitPixel, 1, pixels, 0, 0);
    if (momentum != kCGMomentumScrollPhaseNone)
        CGEventSetIntegerValueField(cg, kCGScrollWheelEventMomentumPhase, momentum);
    CGEventSetTimestamp(cg, nowNs());
    inject(cg);
}

// --- engine side -----------------------------------------------------------------------------

io::IInputDeviceManager* g_manager = 0;

io::IKeyboardDevice* keyboard()
{ return static_cast<io::IKeyboardDevice*>(g_manager->getInputDevice(io::IDFKeyboard)); }
io::IAdvancedInputDevice* mouse()
{ return static_cast<io::IAdvancedInputDevice*>(g_manager->getInputDevice(io::IDFMouse)); }

// One frame: the raw-mouse drain at the end of pumpEvents, then one InputManager::update with
// one input tick: the poll, then the final pass (10.0).
void frame()
{
    CocoaHostTestHooks::pumpRawMouseForTest(ClientHost::instance());
    g_manager->update(-1.0f);
    g_manager->update(10.0f);
}

// A GCMouse report as MacRawMouse's mouseMovedHandler publishes it (y already down positive).
void raw(float dx, float dy, int offsetMs = 0)
{
    macos::rawMousePublish(dx, dy, ClientHost::tickCountMs() + offsetMs);
}

// The focus part of BF2::handleInGameWinMessages: setHasFocus(window focus, mask), the mouse bit
// cleared while a menu logic exists (always, on the client).
void engineFocus(bool focus)
{
    const unsigned int mask = static_cast<unsigned int>(-2);
    if (focus) g_manager->activateDevices(mask);
    else       g_manager->deActivateDevices(mask);
}

void setFocus(bool focus)
{
    CocoaHostTestHooks::overrideFocusForTest(ClientHost::instance(), focus, focus);
    engineFocus(focus);
}

// Clears every device: releases all keys and buttons and runs two frames.
void settle()
{
    frame(); frame();
}

// Records the mouse events every host listener sees, as BF2Window_native's WindowMessageSource does.
struct MouseLog final : host::IHostEventListener
{
    int moves = 0, buttons = 0, wheels = 0;
    int capturedMoves = 0, capturedButtons = 0, capturedWheels = 0;
    void onMouseMove(const host::HostMouseMoveEvent& e) override { ++moves; capturedMoves += e.captured; }
    void onMouseButton(const host::HostMouseButtonEvent& e) override { ++buttons; capturedButtons += e.captured; }
    void onMouseWheel(const host::HostMouseWheelEvent& e) override { ++wheels; capturedWheels += e.captured; }
    double rawX = 0.0, rawY = 0.0;
    void onRawMouseMove(const host::HostRawMouseEvent& e) override { rawX += e.dx; rawY += e.dy; }
    void reset() { *this = MouseLog(); }
};

// Records the focus notifications every host listener sees, and whether a button event arrived
// after the notification that opened the focus gate.
struct FocusLog final : host::IHostEventListener
{
    int activations = 0, deactivations = 0, focusChanges = 0, buttons = 0, buttonsWithFocus = 0;
    bool key = false, active = false;
    void onAppActivationChanged(bool appActive) override { ++(appActive ? activations : deactivations); }
    void onFocusChanged(bool windowKey, bool appActive) override { ++focusChanges; key = windowKey; active = appActive; }
    void onMouseButton(const host::HostMouseButtonEvent&) override { ++buttons; buttonsWithFocus += key && active; }
    void reset() { activations = deactivations = focusChanges = buttons = buttonsWithFocus = 0; }
};

// --- tests -----------------------------------------------------------------------------------

// Every key the shipped Controls.con files bind (mods/bf2 and mods/xpack, Settings and Default)
// is reachable from a macOS key code, and a press and release arrive as DirectInput reports them.
void testControlsConKeys()
{
    begin("Controls.con keys reach their DIK_ codes");
    struct K { const char* name; unsigned char dik; };
    static const K keys[] = {
        {"0",11},{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{"7",8},{"8",9},{"9",10},
        {"A",30},{"B",48},{"C",46},{"D",32},{"E",18},{"F",33},{"G",34},{"J",36},{"K",37},{"L",38},
        {"M",50},{"N",49},{"P",25},{"Q",16},{"R",19},{"S",31},{"T",20},{"V",47},{"W",17},{"X",45},
        {"Z",44},{"Add",78},{"ArrowDown",208},{"ArrowLeft",203},{"ArrowRight",205},{"ArrowUp",200},
        {"Backspace",14},{"Delete",211},{"End",207},{"Enter",28},{"Escape",1},{"F1",59},{"F2",60},
        {"F3",61},{"F4",62},{"F5",63},{"F6",64},{"F7",65},{"F8",66},{"F9",67},{"F10",68},{"F11",87},
        {"F12",88},{"Grave",41},{"Home",199},{"Insert",210},{"PageDown",209},{"PageUp",201},
        {"PrintScreen",183},{"Space",57},{"Tab",15},
    };
    for (const K& k : keys)
    {
        const unsigned short code = macos::macKeyCodeFromDik(k.dik, false);
        char what[96];
        std::snprintf(what, sizeof what, "IDKey_%s (DIK %u) has a macOS key code", k.name, k.dik);
        check(code != 0xFFFF, what);
        if (code == 0xFFFF) continue;
        key(code, true);
        frame();
        std::snprintf(what, sizeof what, "IDKey_%s pressed after its key down", k.name);
        check(keyboard()->keyPressed(static_cast<io::InputDeviceKeys>(k.dik)), what);
        key(code, false);
        frame();
        std::snprintf(what, sizeof what, "IDKey_%s released after its key up", k.name);
        check(keyboard()->keyReleased(static_cast<io::InputDeviceKeys>(k.dik)) &&
              !keyboard()->keyDown(static_cast<io::InputDeviceKeys>(k.dik)), what);
    }
    settle();

    // Modifiers come from flagsChanged with the device-dependent bits.
    struct M { const char* name; unsigned short code; CGEventFlags down; unsigned char dik; };
    static const M mods[] = {
        {"LeftShift", 0x38, kCGEventFlagMaskShift | 0x02, 42},
        {"RightShift", 0x3C, kCGEventFlagMaskShift | 0x04, 54},
        {"LeftCtrl", 0x3B, kCGEventFlagMaskControl | 0x01, 29},
        {"RightCtrl", 0x3E, kCGEventFlagMaskControl | 0x2000, 157},
        {"LeftAlt", 0x3A, kCGEventFlagMaskAlternate | 0x20, 56},
        {"RightAlt", 0x3D, kCGEventFlagMaskAlternate | 0x40, 184},
    };
    for (const M& m : mods)
    {
        char what[96];
        flags(m.code, m.down);
        frame();
        std::snprintf(what, sizeof what, "IDKey_%s pressed by flagsChanged", m.name);
        check(keyboard()->keyPressed(static_cast<io::InputDeviceKeys>(m.dik)), what);
        flags(m.code, 0);
        frame();
        std::snprintf(what, sizeof what, "IDKey_%s released by flagsChanged", m.name);
        check(keyboard()->keyReleased(static_cast<io::InputDeviceKeys>(m.dik)), what);
    }
    // Caps Lock (IDKey_Capital): a toggle; each change is a press, the release follows next pump.
    flags(0x39, kCGEventFlagMaskAlphaShift);
    frame();
    check(keyboard()->keyPressed(io::IDKey_Capital), "IDKey_Capital pressed when Caps Lock locks");
    inject(CGEventCreate(0));                   // any event: the pump flushes the pending release first
    frame();
    check(keyboard()->keyReleased(io::IDKey_Capital), "IDKey_Capital released on the next pump");
    flags(0x39, 0);
    frame();
    check(keyboard()->keyPressed(io::IDKey_Capital), "IDKey_Capital pressed again when Caps Lock unlocks");
    settle();
}

// DirectInput reports transitions only: auto-repeat key-downs are not device data.
void testKeyRepeat()
{
    begin("auto-repeat is not device data");
    key(kVK_ANSI_W, true);
    frame();
    check(keyboard()->keyPressed(io::IDKey_W), "W pressed");
    for (int i = 0; i < 5; ++i) { key(kVK_ANSI_W, true, true); frame(); }
    check(keyboard()->keyDown(io::IDKey_W) && !keyboard()->keyPressed(io::IDKey_W),
          "W held, not re-pressed, through repeats");
    key(kVK_ANSI_W, false);
    frame();
    check(keyboard()->keyReleased(io::IDKey_W), "W released");
    settle();
}

// The key left of 1 is DIK_GRAVE (the console key) on ANSI and ISO keyboards; the ISO key beside
// left Shift is DIK_OEM_102. macOS swaps the two key codes on ISO keyboards.
void testIsoSwap()
{
    begin("ISO keyboards: console key and OEM_102 by position");
    key(kVK_ANSI_Grave, true, false, kKeyboardANSI);
    frame();
    check(keyboard()->keyPressed(io::IDKey_Grave), "ANSI key left of 1 is DIK_GRAVE");
    key(kVK_ANSI_Grave, false, false, kKeyboardANSI);
    frame();
    key(kVK_ISO_Section, true, false, kKeyboardISO);
    frame();
    check(keyboard()->keyPressed(io::IDKey_Grave), "ISO key left of 1 (kVK_ISO_Section) is DIK_GRAVE");
    key(kVK_ISO_Section, false, false, kKeyboardISO);
    frame();
    key(kVK_ANSI_Grave, true, false, kKeyboardISO);
    frame();
    check(keyboard()->keyPressed(io::IDKey_OEM_102), "ISO key beside left Shift is DIK_OEM_102");
    key(kVK_ANSI_Grave, false, false, kKeyboardISO);
    settle();
}

// Events are consumed by timestamp within a frame's passes (KeyboardDevice::updateCurrentState).
void testSubFrameSampling()
{
    begin("buffered key events are split across input ticks by timestamp");
    key(kVK_ANSI_A, true, false, kKeyboardANSI, 0, -60);
    key(kVK_ANSI_A, false, false, kKeyboardANSI, 0, -10);
    g_manager->update(-1.0f);
    g_manager->update(0.03f);
    check(keyboard()->keyDown(io::IDKey_A), "first pass sees only the press");
    g_manager->update(10.0f);
    check(!keyboard()->keyDown(io::IDKey_A) && keyboard()->keyReleased(io::IDKey_A),
          "last pass sees the release");
    settle();
}

// Focus loss: the window loses focus, the engine deactivates the keyboard and every held key is
// released (KeyboardDevice::deActivate); keys held across a loss/regain inside one pump are dropped
// on the next poll (DIERR_INPUTLOST).
void testFocusLoss()
{
    begin("focus loss clears held keys and buttons");
    key(kVK_ANSI_W, true);
    button(0, true);
    frame();
    check(keyboard()->keyDown(io::IDKey_W) && mouse()->buttonDown(io::IDButton_0), "W and left button held");
    setFocus(false);
    frame();
    check(!keyboard()->keyDown(io::IDKey_W), "W released by deactivation");
    check(!mouse()->buttonDown(io::IDButton_0), "left button released (input lost)");
    key(kVK_ANSI_W, false);                     // the release happens elsewhere
    button(0, false);
    setFocus(true);
    frame();
    check(!keyboard()->keyDown(io::IDKey_W) && !keyboard()->keyPressed(io::IDKey_W), "no stale W after regain");

    key(kVK_ANSI_S, true);
    CocoaHostTestHooks::overrideFocusForTest(ClientHost::instance(), false, false);   // lost and regained within one pump
    CocoaHostTestHooks::overrideFocusForTest(ClientHost::instance(), true, true);
    frame();
    check(!keyboard()->keyDown(io::IDKey_S), "a press before a loss inside one pump is dropped");
    key(kVK_ANSI_S, false);
    settle();
}

// Mouse: raw relative counts, Y down, buttons 0..7 as DIMOFS_BUTTON0..7, sample latency on the
// event clock.
void testMouseBasics()
{
    begin("mouse deltas, buttons and latency");
    move(5, -3);
    move(2, 4);
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 7.0f, "X sums deltas (7)");
    check(mouse()->readAxis(io::IDAxis_1, true) == 1.0f, "Y sums deltas, down positive (1)");
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 0.0f, "axes are per sample");
    const io::InputDeviceButtons bits[5] = { io::IDButton_0, io::IDButton_1, io::IDButton_2,
                                             io::IDButton_3, io::IDButton_4 };
    for (int b = 0; b < 5; ++b)
    {
        char what[64];
        button(b, true);
        frame();
        std::snprintf(what, sizeof what, "button %d is IDButton_%d", b, b);
        check(mouse()->buttonPressed(bits[b]), what);
        button(b, false);
        frame();
        check(mouse()->buttonReleased(bits[b]), "and releases");
    }
    button(0, true, -15);
    frame();
    const float latency = mouse()->buttonPressedSampleLatency(io::IDButton_0);
    check(latency > 0.005f && latency < 0.03f, "press latency measured on the event clock (~15 ms)");
    button(0, false);
    settle();
}

// A wheel notch is +120 away from the user, -120 towards (DirectInput lZ, WHEEL_DELTA).
void testWheelNotches()
{
    begin("wheel notches are +-120");
    wheelLines(1);
    frame();
    check(mouse()->readAxis(io::IDAxis_2, true) == 120.0f, "one notch away is +120");
    wheelLines(-1);
    frame();
    check(mouse()->readAxis(io::IDAxis_2, true) == -120.0f, "one notch towards is -120");
    settle();
}

// Trackpad scrolling: DirectInput's wheel reports whole notches, and Controls.con turns any
// positive or negative wheel sample into c_PINextItem / c_PIPrevItem (addAxisToTriggerMapping,
// ControlMap::axisToTrigger fires on value > 0). A 40-point swipe is four notches; it must not
// select the next item on every sample it spans.
void testPreciseScroll()
{
    begin("trackpad scrolling reports whole notches");
    int samples = 0, nonNotch = 0;
    float total = 0.0f;
    for (int i = 0; i < 20; ++i)
    {
        wheelPixels(2);
        frame();
        const float z = mouse()->readAxis(io::IDAxis_2, true);
        if (z != 0.0f) ++samples;
        if (z != 0.0f && std::fabs(z) != 120.0f) ++nonNotch;
        total += z;
    }
    std::printf("  40 points over 20 samples: %d wheel samples, total %.0f\n", samples, total);
    check(samples == 4, "a 40-point swipe fires 4 wheel samples (one per 10 points)");
    check(nonNotch == 0, "each wheel sample is a whole notch");
    check(std::fabs(total) == 480.0f, "the swipe totals 4 notches");
    settle();
}

// The momentum (inertia) phase AppKit synthesizes after the fingers lift is not device motion.
void testMomentumScroll()
{
    begin("momentum scrolling is not wheel input");
    int samples = 0;
    wheelPixels(5, kCGMomentumScrollPhaseBegin);
    frame();
    if (mouse()->readAxis(io::IDAxis_2, true) != 0.0f) ++samples;
    for (int i = 0; i < 30; ++i)
    {
        wheelPixels(10, kCGMomentumScrollPhaseContinue);
        frame();
        if (mouse()->readAxis(io::IDAxis_2, true) != 0.0f) ++samples;
    }
    wheelPixels(0, kCGMomentumScrollPhaseEnd);
    frame();
    std::printf("  31 momentum events: %d wheel samples\n", samples);
    check(samples == 0, "no wheel samples from momentum events");
    settle();
}

// An exclusively acquired DirectInput mouse (MouseDevice's default cooperative level) hides and
// holds the system cursor, so Windows generates no window mouse messages in-game. Host mouse
// events in that state are marked captured for the window-message source; in menus (mouse
// deactivated by MainMenuLogic::activate) they are not.
void testCapturedMouse(MouseLog& log)
{
    begin("in-game mouse events are captured, menu mouse events are not");
    bool hidden = false, disassociated = false;
    log.reset();
    move(3, 3);
    button(0, true);
    button(0, false);
    wheelLines(1);
    frame();
    CocoaHostTestHooks::cursorStateForTest(ClientHost::instance(), hidden, disassociated);
    check(hidden && disassociated, "in-game: cursor hidden and held (relative mode)");
    check(log.moves == 1 && log.buttons == 2 && log.wheels == 1, "in-game: the host reports the events");
    check(log.capturedMoves == 1 && log.capturedButtons == 2 && log.capturedWheels == 1,
          "in-game: every mouse event is captured");

    mouse()->deActivate();                      // MainMenuLogic::activate
    log.reset();
    move(3, 3);
    button(0, true);
    button(0, false);
    wheelLines(1);
    frame();
    CocoaHostTestHooks::cursorStateForTest(ClientHost::instance(), hidden, disassociated);
    check(!hidden && !disassociated, "menu: cursor visible and free");
    check(log.capturedMoves == 0 && log.capturedButtons == 0 && log.capturedWheels == 0,
          "menu: no mouse event is captured");
    check(log.moves == 1 && log.buttons == 2, "menu: the window still gets moves and clicks");

    mouse()->activate();                        // MainMenuLogic::deactivate
    settle();
}

// With a GCMouse connected the in-game counts are its unaccelerated deltas, summed over every
// mouse; the NSEvent deltas (accelerated) are ignored. Raw motion never reaches window listeners.
void testRawMouse(MouseLog& log)
{
    begin("GCMouse deltas drive the in-game mouse");
    macos::rawMouseSetCountForTest(2);          // two mice
    check(ClientHost::instance().rawMouseAvailable(), "raw mouse available with a GCMouse connected");
    log.reset();
    move(100, 100);                             // the NSEvent of the same motion, accelerated
    raw(5.0f, 3.0f);                            // mouse A
    raw(-2.0f, 1.0f);                           // mouse B
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 3.0f, "X is the sum of the raw deltas (3)");
    check(mouse()->readAxis(io::IDAxis_1, true) == 4.0f, "Y is the sum of the raw deltas, down positive (4)");
    check(log.moves == 1, "the window still sees the NSEvent move (captured), never raw motion");
    for (int i = 0; i < 4; ++i) raw(0.5f, -0.5f);
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 2.0f && mouse()->readAxis(io::IDAxis_1, true) == -2.0f,
          "sub-count raw motion is carried (4 x 0.5 = 2)");
    raw(0.0f, 0.0f);
    frame();

    // Timestamps split raw motion across input ticks like any buffered event: MouseDevice's first
    // tick takes what is older than the previous poll plus the tick (30 ms).
    frame();
    raw(7.0f, 0.0f, 5);
    raw(1.0f, 0.0f, 60);
    CocoaHostTestHooks::pumpRawMouseForTest(ClientHost::instance());
    g_manager->update(-1.0f);
    g_manager->update(0.03f);
    check(mouse()->readAxis(io::IDAxis_0, true) == 7.0f, "first tick sees the older raw report");
    g_manager->update(10.0f);
    check(mouse()->readAxis(io::IDAxis_0, true) == 1.0f, "last tick sees the newer one");

    // Focus: raw reports arriving without focus are discarded.
    CocoaHostTestHooks::overrideFocusForTest(ClientHost::instance(), false, false);
    raw(50.0f, 50.0f);
    frame();
    CocoaHostTestHooks::overrideFocusForTest(ClientHost::instance(), true, true);
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 0.0f, "raw motion without focus is dropped");

    // Disconnect at run time: back to the NSEvent deltas; pending raw reports are ignored.
    macos::rawMouseSetCountForTest(0);
    check(!ClientHost::instance().rawMouseAvailable(), "no raw mouse after the last disconnect");
    raw(9.0f, 9.0f);
    move(4, 2);
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 4.0f && mouse()->readAxis(io::IDAxis_1, true) == 2.0f,
          "fallback: NSEvent deltas drive the mouse");
    macos::rawMouseSetCountForTest(1);          // reconnect
    move(4, 2);
    raw(1.0f, 1.0f);
    frame();
    check(mouse()->readAxis(io::IDAxis_0, true) == 1.0f, "after a reconnect the raw deltas are used again");
    macos::rawMouseSetCountForTest(0);
    settle();
}

// The handler queue publishes from another thread while the engine thread pumps and samples.
void testRawMouseThreads(MouseLog& log)
{
    log.reset();
    begin("raw mouse publication across threads");
    macos::rawMouseSetCountForTest(1);
    // A burst of 200000 reports (ring wrap-around and the full-ring path), then 20000 in lockstep
    // with the engine thread (each waits for the previous one to reach the device), which the
    // device must accept completely: a burst can overflow its 128-record buffer, as in DX8.
    const int burst = 200000, paced = 20000, total = burst + paced;
    std::atomic<bool> done(false), burstDone(false);
    std::atomic<int> accepted(0);
    std::thread producer([&]() {
        for (int i = 0; i < burst; ++i)
            while (!macos::rawMousePublish(1.0f, -1.0f, ClientHost::tickCountMs()))
                std::this_thread::yield();      // full ring: the handler would drop; the test waits
        while (!burstDone.load()) std::this_thread::yield();
        for (int i = 0; i < paced; ++i)
        {
            macos::rawMousePublish(1.0f, -1.0f, ClientHost::tickCountMs());
            while (accepted.load() <= i) std::this_thread::yield();
        }
        done.store(true);
    });
    double sumX = 0.0, sumY = 0.0;
    int frames = 0;
    while (log.rawX < burst) frame();           // drain the burst; the device may overflow here
    frame(); frame();
    burstDone.store(true);
    for (;;)
    {
        const bool finished = done.load();
        frame();
        sumX += mouse()->readAxis(io::IDAxis_0, true);
        sumY += mouse()->readAxis(io::IDAxis_1, true);
        accepted.store(static_cast<int>(sumX));
        ++frames;
        if (finished)
            break;
    }
    producer.join();
    frame();
    sumX += mouse()->readAxis(io::IDAxis_0, true);
    sumY += mouse()->readAxis(io::IDAxis_1, true);
    std::printf("  host delivered %.0f raw; %d samples over %d frames: sum (%.0f, %.0f), %u full-ring retries\n", log.rawX, total, frames,
                sumX, sumY, macos::rawMouseDropped());
    // The device itself clears on a DirectInput buffer overflow (128 records per poll), as DX8
    // does, and this producer runs far faster than any mouse, so the device keeps only part.
    check(log.rawX == total && log.rawY == -total, "every published report reaches the engine thread once, in pairs");
    check(sumX == -sumY, "X and Y stay paired in the device");
    check(sumX == paced && sumY == -paced, "the paced reports all reach the device's axes");
    macos::rawMouseSetCountForTest(0);
    settle();
}

static void testAsyncEnter()
{
    begin("asynchronous Enter held/consuming press bits");
    ClientHost& host = ClientHost::instance();
    (void)host.consumeAsyncEnterState();
    key(kVK_Return, true);
    check(host.consumeAsyncEnterState() == 0x8001, "first Enter query sees held and press");
    check(host.consumeAsyncEnterState() == 0x8000, "second Enter query consumes the press bit");
    key(kVK_Return, true, true);
    check(host.consumeAsyncEnterState() == 0x8000, "auto-repeat creates no second press");
    CocoaHostTestHooks::overrideFocusForTest(host, false, false); CocoaHostTestHooks::overrideFocusForTest(host, true, true);
    key(kVK_Return, true);
    check(host.consumeAsyncEnterState() == 0x8001, "fresh down after an undelivered off-focus release creates a press");
    key(kVK_Return, false);
    check(host.consumeAsyncEnterState() == 0, "release clears held state");
    key(kVK_Return, true); key(kVK_Return, false);
    check(host.consumeAsyncEnterState() == 1, "release before query retains only the press bit");
    check(host.consumeAsyncEnterState() == 0, "released press bit is consumed too");
    key(kVK_ANSI_KeypadEnter, true);
    check(host.consumeAsyncEnterState() == 0x8001, "keypad Enter contributes to VK_RETURN");
    key(kVK_Return, true); key(kVK_ANSI_KeypadEnter, false);
    check(host.consumeAsyncEnterState() == 0x8001, "either Enter keeps the aggregate held");
    key(kVK_Return, false);
    key(kVK_ANSI_A, true);
    check(host.consumeAsyncEnterState() == 0, "ordinary character does not affect Enter");
    key(kVK_ANSI_A, false);
}

void testHostBoundary()
{
    begin("Cocoa labels and actual window-message text delivery");
    ClientHost& host = ClientHost::instance();
    struct Label { unsigned char dik; const char* text; };
    const Label labels[] = {{56,"Option"}, {184,"Right Option"}, {219,"Command"},
                            {220,"Right Command"}, {69,"Clear"}};
    for (const auto& label : labels)
        check(host.keyDisplayName(label.dik) && std::string(host.keyDisplayName(label.dik)) == label.text,
              "Cocoa retains native key names");
    // controlShortcutDown polls AppKit's global flags; injected events cannot validate that query.
    WindowMessageRecorder messages;
    host.addListener(&messages);
    struct TextKey { unsigned short key, character; CGEventFlags modifiers; };
    const TextKey keys[] = {{kVK_ANSI_C,3,kCGEventFlagMaskControl}, {kVK_ANSI_KeypadEnter,3,0},
                            {kVK_Delete,0x7F,0}, {kVK_ANSI_V,'v',kCGEventFlagMaskCommand}};
    for (const auto& value : keys) {
        CGEventRef event = CGEventCreateKeyboardEvent(0, value.key, true);
        CGEventSetFlags(event, value.modifiers);
        CGEventSetIntegerValueField(event, kCGKeyboardEventKeyboardType, kKeyboardANSI);
        CGEventKeyboardSetUnicodeString(event, 1, &value.character);
        inject(event);
        key(value.key, false);
    }
    check(messages.values(hostmsg::WM_CHAR) == std::vector<unsigned int>({3,13,8,22}),
          "real Cocoa event routing preserves Ctrl-C, keypad Enter, Backspace and Command-V");
    host.removeListener(&messages);
    settle();
}

// The focus gate reads a cache of AppKit's state. With the state AppKit and the system would
// report simulated (CocoaHostTestHooks::setSystemFocusForTest), a stale cache is brought in line
// by the next pump, an active application's window that is not key is made key, a press into the
// window repairs before it is routed, and what the gate drops is counted and reported.
void testFocusRepair()
{
    begin("focus follows AppKit; a press repairs a closed gate");
    ClientHost& host = ClientHost::instance();
    typedef CocoaHostTestHooks Hooks;
    FocusLog focus;
    host.addListener(&focus);
    const int self = static_cast<int>(getpid()), otherApp = 1;
    Hooks::SystemFocus sys = {};
    sys.appActive = sys.windowKey = sys.windowOnScreen = sys.makeKeyWorks = true;
    sys.frontPid = self;
    sys.now = 1000.0;
    const auto stats = [&]() { return Hooks::focusRepairStatsForTest(host); };
    // Hands the simulated state to the host `seconds` later; the window's key status is the host's.
    const auto apply = [&](double seconds) { sys.now += seconds; Hooks::setSystemFocusForTest(host, &sys); };
    const auto pump = [&]() { Hooks::pumpFocusForTest(host); sys.windowKey = stats().windowKey; };
    const auto click = [&]() { button(0, true); button(0, false); sys.windowKey = stats().windowKey; };

    // A callback that never came: the cache says inactive and not key, AppKit says active and key.
    Hooks::overrideFocusForTest(host, false, false);
    apply(0.0);
    focus.reset();
    check(!host.hasFocus(), "stale cache: the gate is closed");
    pump();
    check(host.hasFocus() && host.isAppActive(), "a pump brings the cache in line with AppKit");
    check(focus.activations == 1 && focus.focusChanges == 1 && focus.key && focus.active,
          "the listeners hear the activation and the focus once");
    pump(); pump();
    check(focus.activations == 1 && focus.focusChanges == 1 && focus.deactivations == 0,
          "unchanged state notifies nobody");
    check(stats().makeKeyCalls == 0, "a window that is key is left alone");

    // Application active, window on screen but never made key, cache still "key".
    sys.windowKey = false;
    apply(1.0);
    focus.reset();
    pump();
    check(stats().makeKeyCalls == 1 && stats().windowKey, "a pump makes the game window key");
    check(host.hasFocus() && focus.focusChanges == 0, "the gate stays open, nobody is notified twice");
    pump();
    check(stats().makeKeyCalls == 1, "once it is key nothing more is done");

    // Making it key fails: the cache follows AppKit, the attempt repeats every half second.
    sys.windowKey = false;
    sys.makeKeyWorks = false;
    apply(1.0);
    focus.reset();
    pump();
    check(stats().makeKeyCalls == 2, "another attempt for a window that lost the key status unnoticed");
    check(!host.hasFocus() && host.isAppActive() && focus.focusChanges == 1 && !focus.key && focus.active
              && focus.deactivations == 0,
          "the cache follows AppKit: not key, application still active");
    pump(); pump();
    check(stats().makeKeyCalls == 2, "not on every pump");
    apply(0.6);
    pump();
    check(stats().makeKeyCalls == 3, "again after half a second");

    // Another window of this process is due the key status: it keeps it.
    sys.makeKeyWorks = true;
    sys.otherKeyWindow = true;
    apply(1.0);
    pump();
    check(stats().makeKeyCalls == 3 && !host.hasFocus(), "an alert of this process keeps the key status");
    sys.otherKeyWindow = false;
    apply(1.0);
    focus.reset();
    pump();
    check(stats().makeKeyCalls == 4 && host.hasFocus() && focus.focusChanges == 1 && focus.key,
          "without it the window is made key and the gate opens");
    pump();

    // The window gives the key status away while the application stays active (Spotlight): pumps
    // leave it there, a press into the window takes it back and is delivered.
    sys.windowKey = false;
    apply(1.0);
    Hooks::focusCallbackForTest(host, Hooks::WindowResignedKey);
    check(!host.hasFocus() && host.isAppActive(), "resigned key: the gate closes");
    for (int i = 0; i < 3; ++i) { apply(1.0); pump(); }
    check(stats().makeKeyCalls == 4 && !host.hasFocus(),
          "a pump does not take the keyboard back from a panel of another process");
    focus.reset();
    button(0, true);
    sys.windowKey = stats().windowKey;
    check(stats().makeKeyCalls == 5 && host.hasFocus(), "a press into the game window makes it key");
    check(focus.buttons == 1 && focus.buttonsWithFocus == 1 && focus.focusChanges == 1,
          "and is delivered, after the focus change");
    button(0, false);
    check(focus.buttons == 2, "its release pairs up");

    // A modal window of this process: the press is not the game's. It is dropped and reported,
    // one report a second at most.
    sys.windowKey = false;
    sys.modalWindow = true;
    apply(2.0);
    Hooks::focusCallbackForTest(host, Hooks::WindowResignedKey);
    const Hooks::FocusRepairStats before = stats();
    focus.reset();
    click();
    check(focus.buttons == 0 && !host.hasFocus() && stats().makeKeyCalls == before.makeKeyCalls,
          "a press behind a modal window is not delivered and takes nothing");
    check(stats().droppedInputs == before.droppedInputs + 1 && stats().dropReports == before.dropReports + 1,
          "the dropped press is counted and reported");
    for (int i = 0; i < 4; ++i) click();
    key(kVK_ANSI_W, true);
    key(kVK_ANSI_W, false);
    move(1, 1);
    check(stats().droppedInputs == before.droppedInputs + 7 && stats().dropReports == before.dropReports + 1,
          "presses, key presses and moves are counted; one report a second at most");
    apply(1.5);
    click();
    check(stats().dropReports == before.dropReports + 2, "the next second reports again");
    sys.modalWindow = false;

    // The application resigns active and no callback says so: the next pump notices.
    sys.windowKey = true;
    apply(2.0);
    Hooks::focusCallbackForTest(host, Hooks::WindowBecameKey);
    check(host.hasFocus(), "key again");
    sys.appActive = false;
    sys.windowKey = false;
    sys.frontPid = otherApp;
    apply(1.0);
    focus.reset();
    pump();
    check(!host.hasFocus() && !host.isAppActive() && focus.deactivations == 1 && focus.focusChanges == 1,
          "a deactivation nobody reported is noticed by the next pump, once");
    click();
    check(focus.buttons == 0 && !stats().systemActive && stats().makeKeyCalls == before.makeKeyCalls,
          "the click that brings the application forward from another one is not game input");

    // The system calls this process frontmost, AppKit never activates it: a press uses the
    // system's state once that has lasted a quarter of a second.
    sys.frontPid = self;
    apply(1.0);
    pump();
    focus.reset();
    click();
    check(focus.buttons == 0 && !stats().systemActive, "not at once: the application may just be coming forward");
    apply(0.3);
    pump();
    button(0, true);
    check(stats().systemActive && host.hasFocus() && host.isAppActive(), "then the press uses the system's state");
    check(focus.buttons == 1 && focus.buttonsWithFocus == 1 && focus.activations == 1 && focus.focusChanges == 1,
          "and is delivered, after the activation and the focus change");
    button(0, false);
    sys.frontPid = 0;
    apply(1.0);
    pump();
    check(stats().systemActive && host.hasFocus(), "a moment without a frontmost application is not the user leaving");
    sys.frontPid = otherApp;
    apply(1.0);
    focus.reset();
    pump();
    check(!stats().systemActive && !host.hasFocus() && focus.deactivations == 1,
          "another application in front ends that at the next pump");

    // Focus set by a test stands, as before: no simulated system, no repair.
    Hooks::setSystemFocusForTest(host, 0);
    Hooks::overrideFocusForTest(host, false, false);
    focus.reset();
    Hooks::pumpFocusForTest(host);
    button(0, true);
    button(0, false);
    check(!host.hasFocus() && focus.focusChanges == 0 && focus.buttons == 0,
          "overrideFocusForTest alone: pumps and presses leave the forced focus alone");
    host.removeListener(&focus);
    setFocus(true);
    settle();
}

// ProcessAncestry.h: the walk up the parent chain that decides whether the application in front
// started this process, against a process table and against this process's real parent.
void testProcessAncestry()
{
    begin("process ancestry: the parent chain up to launchd");
    using namespace macos;
    // launchd (1) -> 10 -> 20 -> 30 -> 40 -> this process: 40 is the parent.
    const auto table = [](pid_t pid) -> pid_t
    { return pid == 40 ? 30 : pid == 30 ? 20 : pid == 20 ? 10 : pid == 10 ? 1 : -1; };
    check(processAncestry(40, 40, table) == AncestryYes, "the parent is an ancestor");
    check(processAncestry(10, 40, table) == AncestryYes, "so is the process four levels up");
    check(processAncestry(50, 40, table) == AncestryNo, "a process outside the chain is not");
    check(processAncestry(1, 40, table) == AncestryNo && processAncestry(0, 40, table) == AncestryNo,
          "launchd and the kernel are nobody's launcher");
    check(processAncestry(10, 1, table) == AncestryNo, "a process adopted by launchd has no launcher left");
    // The lookup fails above the parent.
    const auto denied = [](pid_t) -> pid_t { return -1; };
    check(processAncestry(30, 40, denied) == AncestryUnknown, "a failed lookup leaves the answer unknown");
    check(processAncestry(40, 40, denied) == AncestryYes, "the parent is recognised without any lookup");
    const auto loop = [](pid_t pid) -> pid_t { return pid == 40 ? 30 : 40; };
    check(processAncestry(10, 40, loop) == AncestryUnknown, "a chain that never reaches launchd is given up");
    // The kernel's process table, as the client reads it.
    check(parentPidOf(getpid()) == getppid(), "sysctl answers this process's parent");
    check(processAncestry(getppid(), getppid(), parentPidOf) == AncestryYes &&
          processAncestry(getpid(), getppid(), parentPidOf) == AncestryNo,
          "the real chain: the parent is an ancestor, this process is not its own");
}

// The launch activation with the system simulated (SystemFocus::activation). The application in
// front at the first request decides what follows. A launcher (the parent or another ancestor of
// this process), nobody, and this process itself lose the front to a request every half second
// and, after a second, to a reset through the Dock. An unrelated application gets the one
// cooperative request and keeps the front. Requests and Dock resets are counted, none is made.
void testLaunchActivation()
{
    begin("launch activation: only a launcher loses the front to repeated requests");
    ClientHost& host = ClientHost::instance();
    typedef CocoaHostTestHooks Hooks;
    // A client started from Terminal through a script: Terminal -> login -> shell -> script ->
    // this process. The editor and the browser are applications outside that chain.
    const int self = static_cast<int>(getpid());
    const int terminal = 484, login = 663, shell = 665, script = 59564;
    const int editor = 439, browser = 600, dock = 511;
    Hooks::SystemFocus sys = {};
    sys.windowOnScreen = sys.makeKeyWorks = true;
    sys.activation = true;
    sys.parentPid = script;
    sys.ancestors[0] = shell;
    sys.ancestors[1] = login;
    sys.ancestors[2] = terminal;
    sys.dockPid = dock;
    sys.now = 5000.0;
    const auto stats = [&]() { return Hooks::activationStatsForTest(host); };
    const auto fallback = [&]() { return Hooks::focusRepairStatsForTest(host).systemActive; };
    // Hands the simulated state to the host `seconds` later.
    const auto apply = [&](double seconds) { sys.now += seconds; Hooks::setSystemFocusForTest(host, &sys); };
    const auto pump = [&]() { Hooks::pumpFocusForTest(host); sys.windowKey = Hooks::focusRepairStatsForTest(host).windowKey; };
    const auto after = [&](double seconds) { apply(seconds); pump(); };
    const auto click = [&]() { button(0, true); button(0, false); };
    const auto appKitActive = [&]() { sys.appActive = sys.windowKey = true; sys.frontPid = self; };
    // What the scenario under way added to the counters.
    Hooks::ActivationStats base = stats();
    const auto requests = [&]() { return stats().requests - base.requests; };
    const auto retries = [&]() { return stats().retries - base.retries; };
    const auto dockResets = [&]() { return stats().dockResets - base.dockResets; };
    // Ends whatever a scenario left: another application in front, twenty seconds later.
    const auto finish = [&]()
    {
        sys.appActive = sys.windowKey = false;
        sys.frontPid = browser;
        after(20.0);
    };
    // A start: the game window is shown inactive with `front` in front, which makes the first
    // request.
    const auto start = [&](int front)
    {
        finish();
        sys.frontPid = front;
        after(1.0);
        base = stats();
        Hooks::beginActivationForTest(host);
    };
    Hooks::overrideFocusForTest(host, false, false);

    // An unrelated application in front: the user works there while the client starts.
    start(editor);
    check(stats().pending && requests() == 1 && retries() == 0 && dockResets() == 0,
          "an unrelated application in front: the one cooperative request is made");
    for (double step : { 0.1, 0.5, 0.6, 1.0, 2.0, 3.0 })
        after(step);                            // 7.2 s after the request
    check(stats().pending && requests() == 1 && retries() == 0 && dockResets() == 0,
          "while it stays in front nothing is repeated and the Dock is left alone");
    check(!host.hasFocus() && !host.isAppActive() && !fallback(), "the game stays without focus");
    after(3.0);
    check(!stats().pending && requests() == 1 && retries() == 0 && dockResets() == 0,
          "after ten seconds the host stops waiting, with that one request made");

    // The same start, and the system grants the request.
    start(editor);
    appKitActive();
    after(0.4);
    check(!stats().pending && retries() == 0 && dockResets() == 0 && host.hasFocus(),
          "the request granted: the game is active and nothing was repeated");

    // The same start; then the system calls this process frontmost and AppKit never activates
    // it. That state is repaired as after a launcher's start, a second after it began.
    start(editor);
    after(3.0);
    sys.frontPid = self;
    after(0.1);
    check(retries() == 0 && dockResets() == 0 && !fallback(),
          "this process frontmost for the system after the wait: no Dock reset at once");
    after(0.5);
    check(retries() == 1 && dockResets() == 0, "the request is repeated within half a second");
    after(0.6);
    check(dockResets() == 1 && fallback() && host.hasFocus(),
          "a second later the system's state is used and the activation reset through the Dock");
    sys.frontPid = dock;
    after(0.1);
    check(stats().pending && retries() == 1 && host.hasFocus(), "the Dock in front does not end the period");
    after(0.25);
    check(retries() == 2, "the request follows three tenths of a second after the reset");
    appKitActive();
    after(0.05);
    check(!stats().pending && !fallback() && host.hasFocus() && dockResets() == 1,
          "AppKit active: the period ends and the system-state fallback is left");

    // The same again, but the requests are refused and the unrelated application is in front
    // once more: it keeps the front, and the host stops trusting the system's state.
    start(editor);
    after(1.0);
    sys.frontPid = self;
    after(0.6);
    after(0.6);
    after(0.6);
    check(fallback() && host.hasFocus() && retries() == 2 && dockResets() == 1,
          "frontmost for the system again: two requests, then the fallback and the Dock reset");
    sys.frontPid = editor;
    after(0.2);
    check(stats().pending && !fallback() && !host.hasFocus(),
          "the unrelated application in front again: the fallback ends, the period waits");
    after(0.6);
    after(0.6);
    check(retries() == 2 && dockResets() == 1, "and nothing more is requested while it stays there");

    // The unrelated application quits and nobody is in front: the front is free.
    start(editor);
    after(2.0);
    sys.frontPid = 0;
    after(0.6);
    check(retries() == 1 && dockResets() == 0, "nobody in front after the wait: the request is repeated");

    // The user goes from the unrelated application to the parent: the parent may lose the front
    // whenever it holds it, as before.
    start(editor);
    after(2.0);
    sys.frontPid = script;
    after(0.6);
    after(0.6);
    after(0.6);
    check(stats().pending && retries() == 2 && dockResets() == 1,
          "the parent in front after the wait: repeated requests and the Dock reset");

    // An ancestor in front that is not the parent: Terminal above the shells and the script.
    start(terminal);
    check(stats().pending && requests() == 1 && retries() == 0, "an ancestor in front: the first request");
    after(0.2);
    check(retries() == 0, "not repeated before half a second has passed");
    after(0.4);
    check(retries() == 1 && dockResets() == 0, "repeated after half a second");
    after(0.5);
    check(retries() == 1 && dockResets() == 1, "after a second the activation is reset through the Dock");
    sys.frontPid = dock;
    after(0.1);
    after(0.25);
    check(retries() == 2 && stats().pending, "and requested again with the Dock in front");
    sys.frontPid = terminal;                    // refused: the launcher is in front again
    after(0.3);
    after(0.3);
    after(0.6);
    check(retries() == 4 && dockResets() == 1,
          "refused: the requests go on every half second, the Dock reset is not repeated");
    appKitActive();
    after(0.1);
    check(!stats().pending && host.hasFocus(), "until AppKit reports the application active");

    // The parent in front: an application that started the client directly.
    start(script);
    after(0.6);
    after(0.6);
    check(requests() == 1 && retries() == 1 && dockResets() == 1,
          "the parent in front: repeated requests and the Dock reset");

    // Nobody in front.
    start(0);
    after(0.6);
    after(0.6);
    check(requests() == 1 && retries() == 1 && dockResets() == 1,
          "nobody in front: repeated requests and the Dock reset");

    // This process in front for the system while AppKit reports it inactive.
    start(self);
    after(0.6);
    check(retries() == 1 && !fallback() && !host.hasFocus(),
          "frontmost for the system, inactive for AppKit: the request is repeated");
    after(0.6);
    check(fallback() && host.hasFocus() && dockResets() == 1,
          "after a second the system's state is used and the activation reset through the Dock");
    sys.frontPid = dock;
    after(0.35);
    appKitActive();
    after(0.1);
    check(!stats().pending && !fallback() && host.hasFocus() && retries() == 2,
          "the request after the reset is granted: active without a switch of applications");

    // Without a Dock process the requests simply go on.
    sys.dockPid = 0;
    start(terminal);
    after(0.6);
    after(0.6);
    check(retries() == 2 && dockResets() == 0 && stats().pending, "no Dock process: requests only");
    sys.dockPid = dock;

    // The user goes to another application after the first request: that ends the period, also
    // when the launcher is in front again afterwards.
    start(terminal);
    after(0.6);
    sys.frontPid = browser;
    after(0.2);
    check(!stats().pending && retries() == 1 && dockResets() == 0,
          "the user switches to another application: the requests end");
    sys.frontPid = terminal;
    after(0.6);
    after(0.6);
    check(!stats().pending && retries() == 1 && dockResets() == 0, "and do not resume behind the user's back");
    start(editor);
    sys.frontPid = browser;
    after(0.3);
    check(!stats().pending, "a switch away from the unrelated application ends the wait too");
    sys.frontPid = 0;
    after(0.6);
    check(retries() == 0 && dockResets() == 0, "nothing is requested after that");

    // The process lookup is denied: an ancestor above the parent cannot be told from an
    // unrelated application and is treated as one. The parent needs no lookup.
    sys.ancestryDenied = true;
    start(terminal);
    after(0.6);
    after(0.6);
    after(1.0);
    check(stats().pending && requests() == 1 && retries() == 0 && dockResets() == 0,
          "lookup denied: an ancestor above the parent gets the one request, like a stranger");
    start(script);
    after(0.6);
    after(0.6);
    check(retries() == 1 && dockResets() == 1, "lookup denied: the parent is still the launcher");
    sys.ancestryDenied = false;

    // A click into the inactive game window is the user choosing the game: the requests start,
    // also against the unrelated application, with one Dock reset for the period.
    start(editor);
    after(2.0);
    click();
    check(stats().pending && retries() == 1, "a click into the inactive game window requests at once");
    after(0.6);
    check(retries() == 2 && dockResets() == 0, "and the requests go on against the application clicked away from");
    after(0.6);
    check(dockResets() == 1, "with the Dock reset a second after the click");
    sys.frontPid = dock;
    apply(0.4);
    click();                                    // while the reset has the Dock in front
    sys.frontPid = editor;
    after(0.6);
    apply(1.5);
    click();
    pump();
    check(retries() == 5 && dockResets() == 1 && stats().pending,
          "further clicks request again and do not repeat the Dock reset");
    after(7.0);
    check(!stats().pending, "ten seconds after the first click the period ends");
    click();
    after(1.2);
    check(stats().pending && dockResets() == 2,
          "a click that finds no period running starts one, with its own Dock reset");

    // The click arrives when the system already calls this process frontmost, and the unrelated
    // application is in front again afterwards: the click chose the game over it all the same.
    start(editor);
    after(2.0);
    sys.frontPid = self;
    apply(0.1);
    click();
    sys.frontPid = editor;
    after(0.6);
    check(stats().pending && retries() == 2,
          "a click with this process frontmost: the application left behind may lose the front too");

    // End the simulation with no period running, and focus as the other tests force it.
    finish();
    check(!stats().pending && !fallback() && !host.hasFocus(), "the last period is over");
    Hooks::setSystemFocusForTest(host, 0);
    setFocus(true);
    settle();
}

// CursorScale.h: points on screen per cursor pixel, from the points one back-buffer pixel covers
// (the view's height in points over the back buffer's height) and MenuViewport::bitmapScale. The
// cursor is bitmapScale back-buffer pixels per cursor pixel wherever that is between 0.5 and 4
// points; both bounds apply to the product.
void testCursorScale()
{
    begin("cursor size: bitmapScale back-buffer pixels per cursor pixel, within 0.5 to 4 points");
    using macos::cursorPointsPerPixel;
    const auto about = [](double a, double b) { return std::fabs(a - b) < 1e-9; };
    check(cursorPointsPerPixel(0.5625, 2) == 1.125,
          "2560x1600 full screen on a 1440x900-point desktop, factor 2: 1.125 points (the approved size)");
    check(about(cursorPointsPerPixel(0.43, 3), 1.29),
          "a full-screen mode above the desktop's backing size (0.43, factor 3): 1.29 points, not 1.5");
    check(about(cursorPointsPerPixel(0.485, 2), 0.97),
          "a 2560x1600 window shrunk to fit the screen (0.485, factor 2): 0.97 points, not 1.0");
    check(cursorPointsPerPixel(0.2, 1) == 0.5, "0.2 points per back-buffer pixel, factor 1: the 0.5-point floor");
    check(cursorPointsPerPixel(0.2, 2) == 0.5 && about(cursorPointsPerPixel(0.3, 2), 0.6),
          "the floor applies after the factor (0.2 x 2 is raised to 0.5, 0.3 x 2 stays 0.6)");
    check(cursorPointsPerPixel(1.0, 1) == 1.0 && cursorPointsPerPixel(1.5, 1) == 1.5,
          "between the bounds the product stands (1.0 and 1.5 points at factor 1)");
    check(cursorPointsPerPixel(0.5, 3) == 1.5, "2940x1912 full screen on a 1470x956-point desktop, factor 3: 1.5 points");
    check(cursorPointsPerPixel(5.0, 1) == 4.0 && cursorPointsPerPixel(3.0, 2) == 4.0 &&
          cursorPointsPerPixel(4.0, 1) == 4.0,
          "never more than 4 points per cursor pixel");
    check(cursorPointsPerPixel(1.0, 0) == 1.0, "a factor of 0 counts as 1");
}

} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, 0, _IONBF, 0);
    @autoreleasepool
    {
        ClientHost& host = ClientHost::instance();
        CocoaHostTestHooks::enableHeadlessForTest(host);
        CocoaHostTestHooks::overrideFocusForTest(host, true, true);
        if (argc == 2 && std::string(argv[1]) == "--async-enter-only")
        {
            testAsyncEnter();
            std::printf("[result] %d checks, %d failures\n", g_checks, g_failures);
            return g_failures ? 1 : 0;
        }
        MouseLog log;
        host.addListener(&log);

        io::installControlMapFixture();
        g_manager = static_cast<io::IInputDeviceManager*>(
            io::NativeInputDeviceManager::create(io::IID_IInputDeviceManager, 0));
        io::inputDeviceManager = g_manager;
        unsigned int mask = 1087;               // BF2Engine::init: inputManager->init(1087)
        const bool all = g_manager->initDevices(&mask);
        std::printf("[input] initDevices(1087) all=%d opened=0x%X\n", all, mask);
        check(!all && (mask & 3u) == 3u, "keyboard and mouse open; the Falcon never does");
        check(!(mask & io::IDFFalcon), "no Falcon on macOS");
        engineFocus(true);
        mouse()->activate();                    // in-game: MainMenuLogic::deactivate activates the mouse
        check(keyboard()->isActive() && mouse()->isActive(), "keyboard and mouse acquired with focus");

        testControlsConKeys();
        testKeyRepeat();
        testIsoSwap();
        testSubFrameSampling();
        testFocusLoss();
        testMouseBasics();
        testWheelNotches();
        testPreciseScroll();
        testMomentumScroll();
        testCapturedMouse(log);
        testRawMouse(log);
        testRawMouseThreads(log);
        testAsyncEnter();

        testHostBoundary();
        testFocusRepair();
        testProcessAncestry();
        testLaunchActivation();
        testCursorScale();

        host.removeListener(&log);
        g_manager->release();
    }
    std::printf("[result] %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

// [TOOLS] What the view tool (docs/lanes/tools.md) asks of the renderer. Not part of retail BF2.
//
// BF2.exe asks the renderer singleton for this interface (IBase::queryInterface(IID_IRendererTools)),
// as for IRendererUi. No export changes: the plugin ABI is untouched.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// Fork-owned interface id; outside the ranges retail's Ids.h uses.
const unsigned int IID_IRendererTools = 0xE6400002u;

// One presented frame of a benchmark (toolsBenchFrame).
struct ToolsBenchFrame
{
    int          tag;            // the run (toolsBenchTag) the frame was drawn for
    float        frameMs;        // from the previous Present's return to this one's
    float        presentWaitMs;  // the time Present blocked
    float        gpuMs;          // the GPU's frame, the sum of its pass spans; -1 when dropped
    unsigned int draws;          // indexed, plain and user-pointer draws
    unsigned int primitives;
};

class IRendererTools
{
public:
    // Requests a shot of the next frame drawn: the back buffer, resolved to one sample, read back
    // either just before the HUD paints (afterHud false: the scene, the sky's second pass and
    // nothing drawn later) or at the end of the scene (afterHud true: with the HUD, without the
    // debug menu and console). Written as pngPath (full size) and jpgPath (jpgWidth wide, aspect
    // kept; full size when the frame is narrower). Paths are absolute; their directory exists.
    // Replaces a request not yet served; null paths cancel it.
    virtual void toolsRequestShot(const char* pngPath, const char* jpgPath, int jpgWidth,
                                  bool afterHud) = 0;
    // 0 while the request waits for its frame, 1 written, -1 failed; the message says what was
    // written (size) or what failed. Valid until the next request.
    virtual int toolsShotState(const char*& message) const = 0;

    // Benchmark recording (docs/lanes/tools.md#3-benchmark-path). Start clears the last
    // recording and turns the GPU pass timers on; from then on each presented frame whose tag is
    // positive is recorded. Tag sets the tag for the frames presented from now on (0: not
    // recorded). Stop ends the recording and turns the timers off unless something else uses
    // them; the records stay readable until the next start. GPU times arrive a few frames after
    // their frame is presented, so read them some frames after the last recorded one.
    virtual void toolsBenchStart() = 0;
    virtual void toolsBenchTag(int tag) = 0;
    virtual void toolsBenchStop() = 0;
    virtual unsigned int toolsBenchFrameCount() const = 0;
    virtual bool toolsBenchFrame(unsigned int index, ToolsBenchFrame& out) const = 0;
    // GPU spans, in first-seen order; a span's time in a frame is -1 when it did not run there.
    virtual unsigned int toolsBenchSpanCount() const = 0;
    virtual const char* toolsBenchSpanName(unsigned int span) const = 0;
    virtual float toolsBenchSpanMs(unsigned int frame, unsigned int span) const = 0;

    // Survey (docs/lanes/tools.md#survey): the next frame's water area is measured, then the
    // report on the loaded level is written to path (absolute; its directory exists). Replaces a
    // request not yet served; null cancels it. State: 0 waiting, 1 written, -1 failed; the
    // message is the path written or what failed. Valid until the next request.
    virtual void toolsRequestSurvey(const char* path) = 0;
    virtual int toolsSurveyState(const char*& message) const = 0;

    // Atmosphere (docs/lanes/tools.md#atmosphere): writes render milestone 11's fit and tables to
    // base.txt and base-<table>.png (base absolute, its directory exists), at once. False when
    // nothing or not everything was written; the message is the file written or what failed.
    // Valid until the next call.
    virtual bool toolsWriteAtmosphere(const char* base, const char*& message) = 0;

protected:
    ~IRendererTools() {}
};

} // namespace dx9rend
} // namespace hfe
} // namespace dice

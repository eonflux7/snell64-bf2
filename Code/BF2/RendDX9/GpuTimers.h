// [DEV] BF2_GPU_TIMERS=<path>: GPU time per renderer pass. Not part of retail BF2.
// Also on while the performance overlay is shown (docs/lanes/ui.md). Off otherwise: every hook
// below is one test of gpuTimersOn().
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

extern bool g_gpuTimersOn;   // BF2_GPU_TIMERS is set (read when RendDX9 loads), or the overlay is shown

inline bool gpuTimersOn() { return g_gpuTimersOn; }

// Starts the span `name` (a string literal): the GPU time from here to the next mark, or to the
// end of the frame, is booked to it. A name marked more than once in a frame adds up.
void gpuTimerMark(const char* name);
// After a successful Present: closes the frame, collects older ones. benchFrame is the
// benchmark record the frame belongs to (ToolsBench.h), or -1.
void gpuTimersFrame(int benchFrame);
void gpuTimersRelease();   // before the device is reset or released

// [UI] The overlay's use: timers run while `live` is set, as well as under BF2_GPU_TIMERS.
void gpuTimersSetLive(bool live);
// [TOOLS] A benchmark's use (ToolsBench.cpp): timers run while it records.
void gpuTimersSetBench(bool on);
struct GpuSpanTime
{
    const char* name;
    float ms;          // smoothed over the last frames collected
};
// The spans of the last frames collected, in first-seen order; "(frame)" is the whole frame.
// Returns how many were written to `out` (at most `max`).
unsigned int gpuTimersSnapshot(GpuSpanTime* out, unsigned int max);
float gpuTimersFrameMs();   // the last collected frame's GPU time, 0 when none

} // namespace dx9rend
} // namespace hfe
} // namespace dice

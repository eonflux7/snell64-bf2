// [TOOLS] The benchmark's frame recorder (docs/lanes/tools.md#3-benchmark-path), renderer side. Not
// part of retail BF2. While no benchmark records, each hook returns after one test.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

extern bool g_toolsBenchOn;   // a benchmark is recording (IRendererTools::toolsBenchStart)

// rendererPresent, just before the swap chain's Present: the frame's draw counts and the start of
// the Present wait.
void toolsBenchBeforePresent();
// rendererPresent, after a successful Present: closes the frame's record. Returns the record's
// index, or -1 when the frame is not recorded (its tag is not positive).
int toolsBenchPresented();
// GpuTimers, when a recorded frame's timestamps are collected: its spans (in milliseconds) and
// the frame's sum.
void toolsBenchGpuSpan(int frame, const char* name, float ms);
void toolsBenchGpuFrame(int frame, float ms);

// IRendererTools' benchmark calls (IRendererTools.h), answered from ToolsCapture.cpp's class.
struct ToolsBenchFrame;
void toolsBenchStart();
void toolsBenchTag(int tag);
void toolsBenchStop();
unsigned int toolsBenchFrameCount();
bool toolsBenchFrame(unsigned int index, ToolsBenchFrame& out);
unsigned int toolsBenchSpanCount();
const char* toolsBenchSpanName(unsigned int span);
float toolsBenchSpanMs(unsigned int frame, unsigned int span);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

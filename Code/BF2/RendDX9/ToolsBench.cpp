// [TOOLS] The benchmark's frame recorder (docs/lanes/tools.md#3-benchmark-path), renderer side. Not
// part of retail BF2.
//
// bench.run (Game/Tools/ViewTool.cpp) starts the recorder, then tags the frames it draws: 0 for the
// warm-up, 1..N for the runs. Each Present of a frame with a positive tag adds a record: the time
// since the previous Present returned (the frame time), how long Present itself blocked (the CPU
// waiting on the GPU or on the swap chain), and the draw and primitive counts of the frame's
// statistics row. GPU time per pass comes from the GPU timers (GpuTimers.cpp), which run while the
// recorder does; their results arrive a few frames late and are filed under the frame that issued
// them. A frame whose timestamps were dropped keeps no GPU time (-1).
#ifdef BF2_IMAGE_RENDDX9

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "GpuTimers.h"
#include "ToolsBench.h"
#include "../RendMisc/IRendererTools.h"

#include <cstring>
#include <string>
#include <vector>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

bool g_toolsBenchOn = false;

namespace {

struct Record
{
    ToolsBenchFrame frame;
    std::vector<float> spanMs;   // per span index, -1 when the span did not run in this frame
};

std::vector<Record>* g_records = 0;
std::vector<std::string>* g_spans = 0;
int g_tag = 0;
double g_ticksPerMs = 0.0;
LONGLONG g_lastPresented = 0;   // QPC when the previous Present returned, 0 before the first
LONGLONG g_presentStart = 0;
unsigned int g_draws = 0;
unsigned int g_primitives = 0;

LONGLONG ticks()
{
    LARGE_INTEGER now;
    ::QueryPerformanceCounter(&now);
    return now.QuadPart;
}

unsigned int spanIndex(const char* name)
{
    for (size_t i = 0; i < g_spans->size(); ++i)
        if ((*g_spans)[i] == name)
            return static_cast<unsigned int>(i);
    g_spans->push_back(name);
    return static_cast<unsigned int>(g_spans->size() - 1);
}

Record* record(int frame)
{
    if (!g_records || frame < 0 || static_cast<size_t>(frame) >= g_records->size())
        return 0;
    return &(*g_records)[frame];
}

} // namespace

void toolsBenchBeforePresent()
{
    if (!g_toolsBenchOn)
        return;
    // The working row of the statistics (words 95..135), which statsBeginFrame rolls into the
    // last-frame row (54..94) at the next frame: indexed, plain and user-pointer draws, primitives.
    const unsigned int* const current =
        reinterpret_cast<const unsigned int*>(static_cast<RendDX9Base*>(g_rendererBase)->renderStats) + 95;
    g_draws = current[18] + current[20] + current[22];
    g_primitives = current[17];
    g_presentStart = ticks();
}

int toolsBenchPresented()
{
    if (!g_toolsBenchOn)
        return -1;
    const LONGLONG now = ticks();
    int index = -1;
    if (g_tag > 0 && g_lastPresented && g_presentStart)
    {
        Record r;
        r.frame.tag = g_tag;
        r.frame.frameMs = static_cast<float>((now - g_lastPresented) / g_ticksPerMs);
        r.frame.presentWaitMs = static_cast<float>((now - g_presentStart) / g_ticksPerMs);
        r.frame.gpuMs = -1.0f;
        r.frame.draws = g_draws;
        r.frame.primitives = g_primitives;
        g_records->push_back(r);
        index = static_cast<int>(g_records->size() - 1);
    }
    g_lastPresented = now;
    g_presentStart = 0;
    return index;
}

void toolsBenchGpuSpan(int frame, const char* name, float ms)
{
    Record* const r = record(frame);
    if (!r)
        return;
    const unsigned int span = spanIndex(name);
    if (r->spanMs.size() <= span)
        r->spanMs.resize(span + 1, -1.0f);
    r->spanMs[span] = ms;
}

void toolsBenchGpuFrame(int frame, float ms)
{
    Record* const r = record(frame);
    if (r)
        r->frame.gpuMs = ms;
}

// IRendererTools' benchmark half (ToolsCapture.cpp holds the class).
void toolsBenchStart()
{
    if (!g_records)
    {
        g_records = new std::vector<Record>();
        g_spans = new std::vector<std::string>();
    }
    if (g_ticksPerMs == 0.0)
    {
        LARGE_INTEGER frequency;
        ::QueryPerformanceFrequency(&frequency);
        g_ticksPerMs = static_cast<double>(frequency.QuadPart) / 1000.0;
    }
    g_records->clear();
    g_spans->clear();
    g_tag = 0;
    g_lastPresented = 0;
    g_presentStart = 0;
    g_toolsBenchOn = true;
    gpuTimersSetBench(true);
}

void toolsBenchTag(int tag)
{
    g_tag = tag;
}

void toolsBenchStop()
{
    g_toolsBenchOn = false;
    g_tag = 0;
    gpuTimersSetBench(false);
}

unsigned int toolsBenchFrameCount()
{
    return g_records ? static_cast<unsigned int>(g_records->size()) : 0;
}

bool toolsBenchFrame(unsigned int index, ToolsBenchFrame& out)
{
    const Record* const r = record(static_cast<int>(index));
    if (!r)
        return false;
    out = r->frame;
    return true;
}

unsigned int toolsBenchSpanCount()
{
    return g_spans ? static_cast<unsigned int>(g_spans->size()) : 0;
}

const char* toolsBenchSpanName(unsigned int span)
{
    return g_spans && span < g_spans->size() ? (*g_spans)[span].c_str() : "";
}

float toolsBenchSpanMs(unsigned int frame, unsigned int span)
{
    const Record* const r = record(static_cast<int>(frame));
    return r && span < r->spanMs.size() ? r->spanMs[span] : -1.0f;
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

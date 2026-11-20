// [TOOLS] tools.survey (docs/lanes/tools.md#survey), renderer side. Not part of retail BF2. While
// no survey is requested, each hook returns after one test.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

extern bool g_toolsSurveyOn;   // a survey waits for its frame (IRendererTools::toolsRequestSurvey)

// Frame.cpp, around the RSMTWater flush: an occlusion query counts water's samples.
void toolsSurveyWaterBegin();
void toolsSurveyWaterEnd();
// rendererPresent, after a successful Present: collects the query and writes the report.
void toolsSurveyPresented();

// IRendererTools' survey calls (IRendererTools.h), answered from ToolsCapture.cpp's class.
void toolsRequestSurvey(const char* path);
int toolsSurveyState(const char*& message);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

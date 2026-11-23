// [TOOLS] tools.atmosphere (docs/lanes/tools.md#atmosphere), renderer side. Not part of retail BF2.
#pragma once

namespace dice {
namespace hfe {
namespace dx9rend {

// IRendererTools::toolsWriteAtmosphere, answered from ToolsCapture.cpp's class.
bool toolsWriteAtmosphere(const char* base, const char*& message);
// IRendererTools::toolsWriteClouds (render milestone 12).
bool toolsWriteClouds(const char* base, const char*& message);
// IRendererTools::toolsWriteExposure (render milestone 13).
bool toolsWriteExposure(const char* base, const char*& message);
// IRendererTools::toolsWriteSun (render milestone 14), in SunStatic.cpp.
bool toolsWriteSun(const char* base, const char*& message);
// IRendererTools::toolsWriteWater (render milestone 23), in WaterOpen.cpp.
bool toolsWriteWater(const char* base, const char*& message);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

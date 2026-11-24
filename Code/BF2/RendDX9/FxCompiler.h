// [RENDER] The effect compiler: d3dcompiler_47's D3DCompile with the fx_2_0 target, in place of
// D3DX's effect compiler (render milestone 15, docs/render/submission.md#15-shader-pipeline).
// The April 2005 compiler in d3dx9_24/25 takes 20 to 30 times as long on the renderer's shaders
// and runs one compile at a time per loaded module; D3DCompile may be called from any thread.
// D3DXCreateEffect (d3dx9_43) loads its output as it loads D3DX's.
#pragma once

#include "../../RendDX9Abi/D3dx9Sdk.h"

namespace dice {
namespace hfe {
namespace dx9rend {

// True when d3dcompiler_47.dll loaded. Without it no effect compiles; cached ones still load.
bool fxCompilerAvailable();

// Compiles effect source to an effect binary for D3DXCreateEffect. flags are D3DXSHADER_* bits;
// effect-only bits (D3DXFX_NOT_CLONEABLE, D3DXFX_LARGEADDRESSAWARE) are dropped. include has
// ID3DInclude's layout. Returns the HRESULT; *blobOut is set on success, *messagesOut (when given)
// receives warnings or errors, without the deprecation warning every effect compile reports.
long fxCompileEffect(const char* source, unsigned int sourceSize, const char* sourceName,
                     const D3DXMACRO* defines, ID3DXInclude* include, unsigned long flags,
                     ID3DXBuffer** blobOut, ID3DXBuffer** messagesOut);

// d3dcompiler_47's D3DPreprocess: the source with its includes and defines expanded (and #line
// directives), for a compile on another thread that needs no include handler. Any thread.
long fxPreprocess(const char* source, unsigned int sourceSize, const char* sourceName,
                  const D3DXMACRO* defines, ID3DXInclude* include, ID3DXBuffer** textOut,
                  ID3DXBuffer** messagesOut);

} // namespace dx9rend
} // namespace hfe
} // namespace dice

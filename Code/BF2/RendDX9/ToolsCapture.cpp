// [TOOLS] The view tool's shots (docs/lanes/tools.md#1-views), renderer side. Not part of retail
// BF2.
//
// A request names two files and a capture point. At that point of the next frame the back buffer
// is copied with StretchRect into a single-sample render target of its format (which resolves a
// multisampled back buffer), read back into system memory, converted to X8R8G8B8 (no alpha in the
// files) and written as a PNG; a box-filtered copy jpgWidth wide is written as a JPEG. Reading the
// back buffer does not depend on the window being visible. The write is synchronous: the frame
// that takes the shot is longer by the encode time.
#ifdef BF2_IMAGE_RENDDX9

#include "ToolsCapture.h"

#include "../../RendDX9Abi/ThunkAbi.h"
#include "../../RendDX9Abi/HfeAbi.h"
#include "../../RendDX9Abi/ModuleCommon.h"
#include "../../RendDX9Abi/D3dx9Sdk.h"
#include "RendDX9Base.h"
#include "../RendMisc/IRendererTools.h"
#include "ToolsBench.h"
#include "ToolsSurvey.h"
#include "ToolsAtmosphere.h"

#include <cstdio>
#include <string>

namespace dice {
namespace hfe {
namespace dx9rend {

extern void* g_rendererBase;

namespace {

struct Request
{
    bool        pending;
    bool        afterHud;
    int         jpgWidth;
    int         state;      // 0 waiting, 1 written, -1 failed
    std::string pngPath;
    std::string jpgPath;
    std::string message;
};

Request g_request = { false, false, 960, 1, std::string(), std::string(), std::string() };

template <class T>
void release(T*& p)
{
    if (p)
        p->Release();
    p = 0;
}

std::string hresultText(const char* what, HRESULT hr)
{
    char text[96];
    std::snprintf(text, sizeof(text), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
    return text;
}

// Returns an empty string on success, else what failed.
std::string capture(RendDX9Base* b)
{
    IDirect3DDevice9* const dev = b->device;
    if (!dev || b->swapChains.empty() || !b->swapChains[0])
        return "no device";

    IDirect3DSurface9* backBuffer = 0;
    IDirect3DSurface9* resolved = 0;
    IDirect3DSurface9* readback = 0;
    IDirect3DSurface9* full = 0;
    IDirect3DSurface9* reduced = 0;
    std::string error;

    HRESULT hr = b->swapChains[0]->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    D3DSURFACE_DESC desc = {};
    if (FAILED(hr))
        error = hresultText("GetBackBuffer", hr);
    else
        backBuffer->GetDesc(&desc);

    if (error.empty())
    {
        hr = dev->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                     FALSE, &resolved, 0);
        if (FAILED(hr))
            error = hresultText("CreateRenderTarget", hr);
    }
    if (error.empty())
    {
        hr = dev->StretchRect(backBuffer, 0, resolved, 0, D3DTEXF_NONE);
        if (FAILED(hr))
            error = hresultText("StretchRect", hr);
    }
    if (error.empty())
    {
        hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM,
                                              &readback, 0);
        if (FAILED(hr))
            error = hresultText("CreateOffscreenPlainSurface", hr);
    }
    if (error.empty())
    {
        hr = dev->GetRenderTargetData(resolved, readback);
        if (FAILED(hr))
            error = hresultText("GetRenderTargetData", hr);
    }
    if (error.empty())
    {
        hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8,
                                              D3DPOOL_SCRATCH, &full, 0);
        if (SUCCEEDED(hr))
            hr = D3DXLoadSurfaceFromSurface(full, 0, 0, readback, 0, 0, D3DX_FILTER_NONE, 0);
        if (SUCCEEDED(hr))
            hr = D3DXSaveSurfaceToFileA(g_request.pngPath.c_str(), D3DXIFF_PNG, full, 0, 0);
        if (FAILED(hr))
            error = hresultText("PNG", hr);
    }
    unsigned int reducedWidth = desc.Width;
    unsigned int reducedHeight = desc.Height;
    if (error.empty())
    {
        if (g_request.jpgWidth > 0 && desc.Width > static_cast<unsigned int>(g_request.jpgWidth))
        {
            reducedWidth = static_cast<unsigned int>(g_request.jpgWidth);
            reducedHeight = (desc.Height * reducedWidth + desc.Width / 2) / desc.Width;
        }
        hr = dev->CreateOffscreenPlainSurface(reducedWidth, reducedHeight, D3DFMT_X8R8G8B8,
                                              D3DPOOL_SCRATCH, &reduced, 0);
        if (SUCCEEDED(hr))
            hr = D3DXLoadSurfaceFromSurface(reduced, 0, 0, full, 0, 0, D3DX_FILTER_BOX, 0);
        if (SUCCEEDED(hr))
            hr = D3DXSaveSurfaceToFileA(g_request.jpgPath.c_str(), D3DXIFF_JPG, reduced, 0, 0);
        if (FAILED(hr))
            error = hresultText("JPEG", hr);
    }

    release(reduced);
    release(full);
    release(readback);
    release(resolved);
    release(backBuffer);

    if (error.empty())
    {
        char text[128];
        std::snprintf(text, sizeof(text), "%ux%u, %s the HUD, JPEG %ux%u", desc.Width, desc.Height,
                      g_request.afterHud ? "with" : "before", reducedWidth, reducedHeight);
        g_request.message = text;
    }
    return error;
}

void serve(bool afterHud)
{
    if (!g_request.pending || g_request.afterHud != afterHud || !g_rendererBase)
        return;
    g_request.pending = false;
    const std::string error = capture(static_cast<RendDX9Base*>(g_rendererBase));
    if (error.empty())
        g_request.state = 1;
    else
    {
        g_request.state = -1;
        g_request.message = error;
    }
}

class RendererTools : public IRendererTools
{
public:
    virtual void toolsRequestShot(const char* pngPath, const char* jpgPath, int jpgWidth,
                                  bool afterHud)
    {
        if (!pngPath || !jpgPath)
        {
            g_request.pending = false;
            g_request.state = -1;
            g_request.message = "cancelled";
            return;
        }
        g_request.pending = true;
        g_request.afterHud = afterHud;
        g_request.jpgWidth = jpgWidth;
        g_request.state = 0;
        g_request.pngPath = pngPath ? pngPath : "";
        g_request.jpgPath = jpgPath ? jpgPath : "";
        g_request.message.clear();
    }

    virtual int toolsShotState(const char*& message) const
    {
        message = g_request.message.c_str();
        return g_request.state;
    }

    // ToolsBench.cpp
    virtual void toolsBenchStart() { dx9rend::toolsBenchStart(); }
    virtual void toolsBenchTag(int tag) { dx9rend::toolsBenchTag(tag); }
    virtual void toolsBenchStop() { dx9rend::toolsBenchStop(); }
    virtual unsigned int toolsBenchFrameCount() const { return dx9rend::toolsBenchFrameCount(); }
    virtual bool toolsBenchFrame(unsigned int index, ToolsBenchFrame& out) const
    {
        return dx9rend::toolsBenchFrame(index, out);
    }
    virtual unsigned int toolsBenchSpanCount() const { return dx9rend::toolsBenchSpanCount(); }
    virtual const char* toolsBenchSpanName(unsigned int span) const
    {
        return dx9rend::toolsBenchSpanName(span);
    }
    virtual float toolsBenchSpanMs(unsigned int frame, unsigned int span) const
    {
        return dx9rend::toolsBenchSpanMs(frame, span);
    }

    // ToolsSurvey.cpp
    virtual void toolsRequestSurvey(const char* path) { dx9rend::toolsRequestSurvey(path); }
    virtual int toolsSurveyState(const char*& message) const
    {
        return dx9rend::toolsSurveyState(message);
    }

    // ToolsAtmosphere.cpp
    virtual bool toolsWriteAtmosphere(const char* base, const char*& message)
    {
        return dx9rend::toolsWriteAtmosphere(base, message);
    }
};

RendererTools g_tools;

} // namespace

IRendererTools* toolsInterface()
{
    return &g_tools;
}

void toolsCaptureBeforeHud()
{
    if (g_request.pending)
        serve(false);
}

void toolsCaptureAfterHud()
{
    if (g_request.pending)
        serve(true);
}

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

// [RENDER] The effect compiler (FxCompiler.h; render milestone 15).

// See Code/BF2/RendDX9/BF2Render.cpp for the BF2_IMAGE_RENDDX9 guard and why it is here.

#ifdef BF2_IMAGE_RENDDX9

#include "FxCompiler.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32) && !defined(__APPLE__)
#include <windows.h>
#endif

namespace dice {
namespace hfe {
namespace dx9rend {

#if defined(_WIN32) && !defined(__APPLE__)
namespace {

// D3DCompile. D3D_SHADER_MACRO, ID3DInclude and ID3DBlob have the layouts of D3DXMACRO,
// ID3DXInclude and ID3DXBuffer (ID3DBlob::GetBufferSize returns SIZE_T; only its low 32 bits
// are read here, through ID3DXBuffer's DWORD).
typedef HRESULT (WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3DXMACRO*, ID3DXInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DXBuffer**, ID3DXBuffer**);

D3DCompileFn compileEntry()
{
    // Resolved once; LoadLibrary and the static's initialisation are thread-safe.
    static const D3DCompileFn entry = []() -> D3DCompileFn {
        const HMODULE module = ::LoadLibraryA("d3dcompiler_47.dll");
        const D3DCompileFn fn = module
            ? reinterpret_cast<D3DCompileFn>(::GetProcAddress(module, "D3DCompile")) : 0;
        std::fprintf(stderr, fn ? "[FXCOMPILER] d3dcompiler_47 D3DCompile fx_2_0\n"
                                : "[FXCOMPILER] d3dcompiler_47.dll not loaded: effects do not compile\n");
        std::fflush(stderr);
        return fn;
    }();
    return entry;
}

// The D3DXSHADER_* bits D3DCompile shares (bit for bit with D3DCOMPILE_*). 0x800 is
// D3DXFX_NOT_CLONEABLE for D3DX but D3DCOMPILE_ENABLE_STRICTNESS for D3DCompile, and bits from
// 0x10000 up are D3DX's own.
const unsigned long kSharedFlags = 0xF7FFu;

// The messages without X4717 ("Effects deprecated for D3DCompiler_47"), or null when nothing else
// is left.
ID3DXBuffer* withoutDeprecation(ID3DXBuffer* messages)
{
    if (!messages)
        return 0;
    const char* const text = static_cast<const char*>(messages->GetBufferPointer());
    const std::string all(text, ::strnlen(text, messages->GetBufferSize()));
    std::string kept;
    std::string::size_type line = 0;
    while (line < all.size())
    {
        std::string::size_type end = all.find('\n', line);
        end = end == std::string::npos ? all.size() : end + 1;
        const std::string piece = all.substr(line, end - line);
        if (piece.find("X4717") == std::string::npos
            && piece.find_first_not_of(" \t\r\n") != std::string::npos)
            kept += piece;
        line = end;
    }
    messages->Release();
    if (kept.empty())
        return 0;
    ID3DXBuffer* out = 0;
    if (D3DXCreateBuffer(static_cast<DWORD>(kept.size() + 1), &out) != 0 || !out)
        return 0;
    std::memcpy(out->GetBufferPointer(), kept.c_str(), kept.size() + 1);
    return out;
}

// D3DPreprocess, same layouts
typedef HRESULT (WINAPI* D3DPreprocessFn)(LPCVOID, SIZE_T, LPCSTR, const D3DXMACRO*, ID3DXInclude*,
                                          ID3DXBuffer**, ID3DXBuffer**);

D3DPreprocessFn preprocessEntry()
{
    static const D3DPreprocessFn entry = []() -> D3DPreprocessFn {
        const HMODULE module = ::LoadLibraryA("d3dcompiler_47.dll");
        return module ? reinterpret_cast<D3DPreprocessFn>(::GetProcAddress(module, "D3DPreprocess")) : 0;
    }();
    return entry;
}

} // anonymous namespace

bool fxCompilerAvailable()
{
    return compileEntry() != 0;
}

long fxPreprocess(const char* source, unsigned int sourceSize, const char* sourceName,
                  const D3DXMACRO* defines, ID3DXInclude* include, ID3DXBuffer** textOut,
                  ID3DXBuffer** messagesOut)
{
    *textOut = 0;
    if (messagesOut)
        *messagesOut = 0;
    const D3DPreprocessFn preprocess = preprocessEntry();
    if (!preprocess)
        return static_cast<long>(0x80004005);                 // E_FAIL
    ID3DXBuffer* messages = 0;
    const HRESULT hr = preprocess(source, sourceSize, sourceName, defines, include, textOut, &messages);
    if (messagesOut)
        *messagesOut = messages;
    else if (messages)
        messages->Release();
    if (FAILED(hr) && *textOut)
    {
        (*textOut)->Release();
        *textOut = 0;
    }
    return hr;
}

long fxCompileEffect(const char* source, unsigned int sourceSize, const char* sourceName,
                     const D3DXMACRO* defines, ID3DXInclude* include, unsigned long flags,
                     ID3DXBuffer** blobOut, ID3DXBuffer** messagesOut)
{
    *blobOut = 0;
    if (messagesOut)
        *messagesOut = 0;
    const D3DCompileFn compile = compileEntry();
    if (!compile)
        return static_cast<long>(0x80004005);                 // E_FAIL
    ID3DXBuffer* messages = 0;
    const HRESULT hr = compile(source, sourceSize, sourceName, defines, include, 0, "fx_2_0",
                               static_cast<UINT>(flags & kSharedFlags), 0, blobOut, &messages);
    messages = withoutDeprecation(messages);
    if (messagesOut)
        *messagesOut = messages;
    else if (messages)
        messages->Release();
    if (FAILED(hr) && *blobOut)
    {
        (*blobOut)->Release();
        *blobOut = 0;
    }
    return hr;
}
#else
bool fxCompilerAvailable()
{
    return false;
}

long fxPreprocess(const char*, unsigned int, const char*, const D3DXMACRO*, ID3DXInclude*,
                  ID3DXBuffer** textOut, ID3DXBuffer** messagesOut)
{
    *textOut = 0;
    if (messagesOut)
        *messagesOut = 0;
    return static_cast<long>(0x80004001);                     // E_NOTIMPL
}

long fxCompileEffect(const char*, unsigned int, const char*, const D3DXMACRO*, ID3DXInclude*,
                     unsigned long, ID3DXBuffer** blobOut, ID3DXBuffer** messagesOut)
{
    *blobOut = 0;
    if (messagesOut)
        *messagesOut = 0;
    return static_cast<long>(0x80004001);                     // E_NOTIMPL
}
#endif

} // namespace dx9rend
} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9

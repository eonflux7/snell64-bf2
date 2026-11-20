// Ra effect compilation, shader definitions, cache and device-reset handling.

#ifdef BF2_IMAGE_RENDDX9

#include "../../../RendDX9Abi/ThunkAbi.h"
#include "../../../RendDX9Abi/HfeAbi.h"
#include "../../Core/LoadTrace.h"   // [DEV] BF2_LOAD_TRACE
#include "../../../RendDX9Abi/ModuleCommon.h"
#include "../../../RendDX9Abi/BfMemory.h"
#include "../../../RendDX9Abi/Frontier.h"
#include "../../../RendDX9Abi/PluginEntry.h"   // dice::hfe::fileManager - the +0x38 getFileInfo probe
#include "../../../RendDX9Abi/D3dx9Sdk.h"
#if defined(_WIN32)
#include <dxerr9.h>
#endif
#include "../RendDX9Base.h"
#include "../ShaderLoading.h"
#include "../../RendMisc/IRenderer.h"   // Renderer +0xBC, typed (see below)
#include "../../Game/Main/IVideoOptions.h"
#include "../../Core/StringUtil.h"
// FOR sub_10033740 only, and it is a read, not a policy change. The block at the
// top of this file explains that the two translation units deliberately meet at the linker; that is
// still true for metaFxConstruct/metaFxParse, which stay externs. sub_10033740 is different: it
// walks the MetaFx cache and drives ONE MetaFx through its public surface (getGeneratedNames() and
// generateShader()), and there is no way to do that through an extern without defining a new body
// in Ra/RaShaderPermutator.cpp - a file this sitting may not edit. Nothing below indexes a MetaFx
// by offset and no private member is touched.
#include "RaShaderPermutator.h"
#include "RaShader.h"          // RaShaderTable + findOrCreateShader, for the load pass
#if !defined(_MSC_VER)
// [CHOSEN, not recovered] Native overloads retain declaration order.
#include "../../IO/File/FileChangeManager.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <string>
#include <list>
#include <map>
#include <regex>
#include <set>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dice {
namespace hfe {

extern IBase* fileChangeManager; // PluginEntry.cpp, PE 0x10244AC8

namespace dx9rend {

// From RendDX9Init.cpp - the DeviceCaps this file interrogates, and the base object.
extern void* g_deviceCaps;
extern void* g_rendererBase;

// BF2Render.cpp, dword_1023D098 - the Renderer singleton (`g_Renderer = this` in its ctor). The
// MetaFx loader guards the path fixup on it and the fixup dispatches its xpack test through it.
// Distinct from g_rendererBase (dword_1023B970); do not substitute one for the other.
extern IBase* g_Renderer;

// RenderQueues.cpp, dword_1023E408 - the 184-byte render-queue table. getMetaFx reads exactly ONE
// byte of it, the +0xB4 flag its constructor sets to 1, and hands it to the loader.
extern void* g_renderQueues;

// StateManager.cpp, dword_1023B984. getEffect hands it to every effect it compiles through
// ID3DXEffect::SetStateManager (+0x11C) - the same attested slot Effect::bind uses.
extern void* g_stateManager;

// Shared in-place shader deobfuscation.
void deobfuscateShader(unsigned char* buf, unsigned int size);

namespace Ra {

extern void* g_effectManager;

// MetaFx construction and parsing are implemented in RaShaderPermutator.cpp.
// Its constructor takes the path stem, extension and mode flag.

class MetaFx;

// The size of the object `metaFxConstruct` builds in `storage`. It is NOT the shipped literal
// 0xC0. Retail measures 0xC0 with a 16-byte std::vector and a 12-byte std::map; this toolchain's
// are 12 and 8, so the same members measure less here. Asking the owning translation unit keeps the
// allocation and the object in agreement on whatever toolchain builds this DLL, which a hard-coded
// 0xC0 would not. See the seam at the bottom of Ra/RaShaderPermutator.cpp.
unsigned int metaFxObjectSize();

// PE 0x1003E110. `storage` is the engine allocation sized by the call above; returns it.
MetaFx* metaFxConstruct(void* storage,
                        const std::string& stem,
                        const std::string& extension,
                        bool flag);

// PE 0x1003EEF0 - `do { sub_1003EAB0(this); } while (!sub_1003E3B0(this));`, the permutation parse
// loop. __thiscall, no arguments.
void metaFxParse(MetaFx* self);


// Reads device capabilities from the D3DCAPS9 record at offset zero.

namespace {

struct CapsView
{
    D3DCAPS9               caps;
    D3DADAPTER_IDENTIFIER9 adapter;
};

const CapsView* capsOf()      { return static_cast<const CapsView*>(g_deviceCaps); }

bool         isNvidia()       { const CapsView* c = capsOf(); return c && c->adapter.VendorId == 4318u; }
bool         isAti()          { const CapsView* c = capsOf(); return c && c->adapter.VendorId == 4098u; }
// The shader-major readers are RendDX9Init.cpp's deviceCapsVertexShaderMajor (retail sub_100010B0)
// and deviceCapsPixelShaderMajor (retail sub_100010D0); the calls below use them directly.

// sub_100010E0 - `*(BYTE*)(this + 204)`, byte 0 of D3DCAPS9.PixelShaderVersion: the minor number.
// The compile search wants the pair as one 0xMMmm word, which is how 0x100354EF..0x10035504 builds
// it (`major << 8 | minor`).
unsigned int psMinor()        { const CapsView* c = capsOf(); return c ? (c->caps.PixelShaderVersion & 0xFF) : 0u; }
// sub_10001450 - `*(DWORD*)(this + 264) & 1`. Byte 264 of D3DCAPS9 is PS20Caps.Caps, and bit 0 is
// the documented D3DPS20CAPS_ARBITRARYSWIZZLE.
bool ps20ArbitrarySwizzle()   { const CapsView* c = capsOf(); return c && (c->caps.PS20Caps.Caps & 1u) != 0; }

// The three trees' head nodes, by the node-size rule BufferManager.cpp records (size == flag
// offset + 3, confirmed against two read allocators):
//     sub_10036120  flag 45  -> 48   [INFERRED] by the rule - the two string -> pointer caches
//     sub_10036250  flag 69  -> 72   [INFERRED] by the rule - the string -> string memory files
//     sub_100362F0  flag 41  -> 44   [INFERRED] by the rule - the include handler's name set

// sub_10036390 / sub_100372D0 (the effect cache) and sub_10035E40 / sub_10035FD0 / sub_10036470
// (the memory files) push their two c_str()s straight into MSVCR71's `_stricmp`.
struct RaStricmpLess
{
    bool operator()(const std::string& a, const std::string& b) const
    {
        return ::_stricmp(a.c_str(), b.c_str()) < 0;
    }
};

typedef std::map<std::string, void*, RaStricmpLess>        RaEffectCache;     // -> ID3DXEffect*
typedef std::map<std::string, MetaFx*>                     RaMetaFxCache;     // strcmp order
typedef std::map<std::string, std::string, RaStricmpLess> RaMemoryFileMap;
typedef std::set<std::string>                              RaWatchSet;        // strcmp order

// Reads the shared vendor-probe transparency-AA code as a 32-bit value.
inline unsigned int transparencyAACode(const unsigned char (&code)[4])
{
    unsigned int value;
    ::memcpy(&value, code, sizeof(value));
    return value;
}


// Include resolver and file-change listener owned by EffectManager.
// Tracks watched names case-sensitively and keeps the current shader's fully expanded text.

void* raIncludeVtable();
void* raIncludeListenerVtable();

struct EffectSubObject
{
    // PE 0x10035A00
    EffectSubObject()
        : vptr(raIncludeVtable()), vptr2(raIncludeListenerVtable()), refCount(1) {}
    ~EffectSubObject();

    void*        vptr;        // +0x00  &off_101E139C - the ID3DXInclude table, installed below
    void*        vptr2;       // +0x04  off_101E1388
    int          refCount;    // +0x08  1
    RaWatchSet   watched;     // +0x0C  the registered-watch name set
    std::string text;        // +0x18  "Generated.fx"
};

// off_101E143C, the DeviceLostResetListener at EffectManager+0x04. Defined
// after resetEffects, which its lost slot tail-jumps to.
void* effectManagerListenerVtable();
void* effectManagerPrimaryVtable();

} // anonymous namespace


// dice::hfe::dx9rend::Ra::EffectManager - 168 bytes (0xA8), ctor sub_10035A60 (0x10035A60).
// Every offset below is one the constructor writes.

class EffectManager
{
public:
    explicit EffectManager(IDirect3DDevice9* device, bool disableShaderCache);
    ~EffectManager();

    // PE 0x1001A010 / 0x10051870.
    int addRef() { return ++m_refCount; }
    int getRef() { return m_refCount; }
    int release();

    void rebuildShaderDefines();   // sub_10033240, EffectManager.cpp:438
    void rebuildDebugDefines();
    void setShowChannel(unsigned int channel);

    // The MetaFx cache at +0x54. See the block after the class bodies for the evidence.
    MetaFx* getMetaFx(const std::string& name);
    // THE name IS taken BY value, and that is read from the epilogue (`retn 20h` = a 28-byte
    // std::string plus the flag dword), not guessed. The callee destroys it.
    MetaFx* loadMetaFx(std::string name, bool flag);

    const std::string& shaderDefines() const { return m_shaderDefines; }

    // The single byte ShaderManager_updateShaderDefines reads.
    bool flag40() const { return m_flag40 != 0; }

    // the in-memory shader-file table at +0x9C. The permutator writes
    // each generated .fx into it under a lowercased name and drops them again afterwards, which is
    // how a generated permutation reaches the D3DX include handler without ever touching disk.
    bool addMemoryFile(const std::string& name, const std::string& contents);
    void removeMemoryFile(const std::string& name);


    // sub_10035900, `retn 4`. The one call the RaShader build makes.
    void* getEffect(const std::string& name);

    // sub_10034F60, `retn 0Ch`. The compile, including the .cfx disk cache.
    void* compileEffect(const std::string& name, int unused, void** effectOut);

    // Compiles source into an ID3DXBuffer containing effect bytecode.
    ID3DXBuffer* compileEffectSource(const char* source, unsigned int sourceLen,
                                     const D3DXMACRO* defines, unsigned long* flags,
                                     ID3DXBuffer** errors);

    // sub_10033DB0, `retn 8`. Reads one effect file whole into an engine-heap, NUL-terminated
    // buffer. Falls back to the in-memory table and then to generating the permutation.
    unsigned int readEffectFile(const std::string& name, unsigned char** bufferOut);

    // sub_10033AA0, `retn 4`. The in-memory table lookup; null on a miss.
    const std::string* findMemoryFile(const std::string& name);

    // sub_100336C0, `retn 0`. Re-parse every cached MetaFx while the memory table is empty.
    void refillMemoryFiles();

    // sub_10033740, `retn 4`. Generate the one permutation whose clean name matches.
    bool generateMissingShader(const std::string& name);

    // sub_10033170, `retn 8`. Write a compiled blob to the .cfx cache, index-XORed.
    bool writeEffectCache(ID3DXBuffer* blob, const std::string& path);
    bool dumpEffectDisassembly(const std::string& filter);   // PE 0x10033860, unreachable in 1.50

    // The include handler needs these three; it runs as a free __stdcall function on m_sub.
    const std::string& debugDefines() const { return m_debugDefines; }
    EffectSubObject*    includeHandler() const { return m_sub; }

    // Resets cached effects without changing the selected debug channel.
    void resetEffects();

    // The DeviceLostResetListener subobject at +0x04, and its owner back from it.
    void* deviceLostListener() { return &m_vptr2; }
    static EffectManager* fromDeviceLostListener(void* listener)
    {
        return reinterpret_cast<EffectManager*>(
            static_cast<unsigned char*>(listener) - offsetof(EffectManager, m_vptr2));
    }

    void setFlag40(unsigned char value) { m_flag40 = value; }

    // The eight debug-toggle setters: when the byte changes it is stored and every cached effect is
    // rebuilt (resetEffects, sub_10034210). These and the device-pointer setter are unreachable in 1.50:
    // no code or data references. [INFERRED] names.
    // PE 0x10034330: byte +0x48.
    void setDisableDiffuseMap(unsigned char value) { if (m_disableDiffuseMap != value) { m_disableDiffuseMap = value; resetEffects(); } }
    // PE 0x10034350: byte +0x49.
    void setDisableBumpMap(unsigned char value) { if (m_disableBumpMap != value) { m_disableBumpMap = value; resetEffects(); } }
    // PE 0x10034370: byte +0x4A.
    void setDisableSpecular(unsigned char value) { if (m_disableSpecular != value) { m_disableSpecular = value; resetEffects(); } }
    // PE 0x10034390: byte +0x4B.
    void setDisableSpecularMap(unsigned char value) { if (m_disableSpecularMap != value) { m_disableSpecularMap = value; resetEffects(); } }
    // PE 0x100343B0: byte +0x4C.
    void setDisableLightmap(unsigned char value) { if (m_disableLightmap != value) { m_disableLightmap = value; resetEffects(); } }
    // PE 0x100343D0: byte +0x4D.
    void setEnableWireframe(unsigned char value) { if (m_enableWireframe != value) { m_enableWireframe = value; resetEffects(); } }
    // PE 0x100343F0: byte +0x4E.
    void setDisableVsPs(unsigned char value) { if (m_disableVsPs != value) { m_disableVsPs = value; resetEffects(); } }
    // PE 0x10034410: byte +0x4F.
    void setDisableVsPs20(unsigned char value) { if (m_disableVsPs20 != value) { m_disableVsPs20 = value; resetEffects(); } }
    // PE 0x10033220: stores the device pointer (+0x38) without AddRef.
    void setDevice(IDirect3DDevice9* device) { m_device = device; }
    // [TOOLS] tools.survey walks the cache (effectManagerForEachEffect).
    const RaEffectCache& effectCache() const { return m_effectCache; }

private:
    void clearMetaFx();

    void*            m_vptr;          // +0x00  off_101E1448
    void*            m_vptr2;         // +0x04  off_101E143C - DeviceLostResetListener
    int              m_refCount;      // +0x08  1
    // Most recently requested effect and its lowercase name, updated on hits and misses.
    std::string     m_name;          // +0x0C
    void*            m_lastEffect;    // +0x28
    // +0x2C: THE effect cache, lowered name -> ID3DXEffect. THE comparator IS `_stricmp`, not the
    // case-sensitive ordering the +0x54 cache uses: sub_10036390 (find) and sub_100372D0
    // (operator[]) both push their two c_str()s straight into MSVCR71's `_stricmp`.
    RaEffectCache    m_effectCache;   // +0x2C
    IDirect3DDevice9* m_device;       // +0x38
    // +0x3C is D3DXCreateEffect's pPool. The constructor zeroes it and nothing in the image ever
    // writes it, so every Ra effect is created with a NULL effect pool - which is legal and means
    // no cross-effect parameter sharing.
    void*            m_effectPool;    // +0x3C
    unsigned char    m_flag40;        // +0x40  <- the byte ShaderManager reads
    unsigned char    m_flag41;        // +0x41
    unsigned char    m_pad42[2];
    EffectSubObject* m_sub;           // +0x44

    // Eight debug toggles, in the order rebuildDebugDefines emits them.
    unsigned char    m_disableDiffuseMap;   // +0x48
    unsigned char    m_disableBumpMap;      // +0x49
    unsigned char    m_disableSpecular;     // +0x4A
    unsigned char    m_disableSpecularMap;  // +0x4B
    unsigned char    m_disableLightmap;     // +0x4C
    unsigned char    m_enableWireframe;     // +0x4D
    unsigned char    m_disableVsPs;         // +0x4E
    unsigned char    m_disableVsPs20;       // +0x4F

    unsigned int     m_showChannel;   // +0x50
    RaMetaFxCache    m_metaFxCache;   // +0x54  lowered .mfx name -> MetaFx (strcmp order)
    unsigned char    m_byte60;        // +0x60  GSDisableShaderCache
    unsigned char    m_byte61;        // +0x61
    unsigned char    m_byte62;        // +0x62  ctor: 1
    unsigned char    m_pad63;
    std::string     m_debugDefines;  // +0x64
    std::string     m_shaderDefines; // +0x80
    RaMemoryFileMap  m_memoryFiles;   // +0x9C  lowered generated-file name -> its source
};

namespace {

// [CHOSEN, not recovered] Typed dispatch over PE table 0x101E1448. The named
// deleting entry keeps its single manual slot on both native and MSVC builds.
class EffectManagerPrimaryFace
{
public:
    virtual int addRef() = 0;
    virtual int getRef() = 0;
    virtual int release() = 0;
    virtual void* queryInterface(unsigned int) const = 0;
    virtual void onFileChange(const std::string&, unsigned int) = 0;
    virtual void* destroy(unsigned int) = 0;
};

// PE 0x1001A010.
int RD9_THUNK em_addRef(void* self RD9_THUNK_EDX)
{
    return static_cast<EffectManager*>(self)->addRef();
}
// PE 0x10051870.
int RD9_THUNK em_getRef(void* self RD9_THUNK_EDX)
{
    return static_cast<EffectManager*>(self)->getRef();
}
// PE 0x1001A020.
int RD9_THUNK em_release(void* self RD9_THUNK_EDX)
{
    return static_cast<EffectManager*>(self)->release();
}
// PE 0x10041990.
void* RD9_THUNK em_queryInterface(void* self RD9_THUNK_EDX, unsigned int iid)
{
    return iid == RD9_IID_IBASE || iid == 0x3C7B54FFu ? self : 0;
}
// PE 0x10034450. Neither callback argument is read.
void RD9_THUNK em_onFileChange(void* self RD9_THUNK_EDX,
                              const std::string&, unsigned int)
{
    static_cast<EffectManager*>(self)->resetEffects();
}
// PE 0x10037D90.
void* RD9_THUNK em_destroy(void* self RD9_THUNK_EDX, unsigned int flags)
{
    static_cast<EffectManager*>(self)->~EffectManager();
    if (flags & 1u)
        ::renddx9abi::engineFree(self);
    return self;
}

void* const kEffectManagerPrimaryVtable[6] =
{
    reinterpret_cast<void*>(&em_addRef),
    reinterpret_cast<void*>(&em_getRef),
    reinterpret_cast<void*>(&em_release),
    reinterpret_cast<void*>(&em_queryInterface),
    reinterpret_cast<void*>(&em_onFileChange),
    reinterpret_cast<void*>(&em_destroy)
};

void* effectManagerPrimaryVtable()
{
    return const_cast<void**>(kEffectManagerPrimaryVtable);
}

} // anonymous namespace

// PE 0x10035A60. The shipped constructor reads GSDisableShaderCache itself (settings slot +0x50,
// default false) after the sub-object allocation; createEffectManager's caller reads the same
// setting with the same default and passes it in.
EffectManager::EffectManager(IDirect3DDevice9* device, bool disableShaderCache)
    : m_vptr(effectManagerPrimaryVtable())
    , m_vptr2(effectManagerListenerVtable())
    , m_refCount(1)
    , m_lastEffect(0)
    , m_device(device)
    , m_effectPool(0)
    , m_flag40(0)
    , m_flag41(0)
    , m_sub(0)
    , m_disableDiffuseMap(0)
    , m_disableBumpMap(0)
    , m_disableSpecular(0)
    , m_disableSpecularMap(0)
    , m_disableLightmap(0)
    , m_enableWireframe(0)
    , m_disableVsPs(0)
    , m_disableVsPs20(0)
    , m_showChannel(0)
    , m_byte60(0)
    , m_byte61(0)
    , m_byte62(1)
    , m_pad63(0)
{
    m_pad42[0] = 0;
    m_pad42[1] = 0;

    // The three trees (+0x2C, +0x54, +0x9C) are constructed empty with the members.

    void* const subStorage = ::renddx9abi::engineAlloc(sizeof(EffectSubObject));   // sub_10035A00
    m_sub = subStorage ? new (subStorage) EffectSubObject : 0;

    // The setting the constructor reads. The original stores it into the byte at +0x60.
    m_byte60 = disableShaderCache ? 1 : 0;

    rebuildShaderDefines();
    rebuildDebugDefines();
}


// PE 0x10033040 (listing read). Eight bools and a channel enum, appended to the string at
// +0x64. Self-contained; no caps, no device, no settings.
void EffectManager::rebuildDebugDefines()
{
    m_debugDefines.clear();

    if (m_disableDiffuseMap)  m_debugDefines += "#define DISABLE_DIFFUSEMAP\n";
    if (m_disableBumpMap)     m_debugDefines += "#define DISABLE_BUMPMAP\n";
    if (m_disableSpecular)    m_debugDefines += "#define DISABLE_SPECULAR\n";
    if (m_disableSpecularMap) m_debugDefines += "#define DISABLE_SPECULARMAP\n";
    if (m_disableLightmap)    m_debugDefines += "#define DISABLE_LIGHTMAP\n";
    if (m_enableWireframe)    m_debugDefines += "#define ENABLE_WIREFRAME\n";
    if (m_disableVsPs)        m_debugDefines += "#define DISABLE_VSPS\n";
    if (m_disableVsPs20)      m_debugDefines += "#define DISABLE_VSPS20\n";

    if (m_showChannel)
    {
        m_debugDefines += "#define SHOW_CHANNEL\n";
        // SIX separate `if`s IN THE original, NOT A switch, and the last one returns while the
        // others fall through. Functionally identical because the values are distinct; reproduced
        // as a chain rather than tidied into a switch.
        if (m_showChannel == 1) m_debugDefines += "#define DIFFUSE_CHANNEL\n";
        if (m_showChannel == 2) m_debugDefines += "#define NORMAL_CHANNEL\n";
        if (m_showChannel == 3) m_debugDefines += "#define SPECULAR_CHANNEL\n";
        if (m_showChannel == 4) m_debugDefines += "#define ALPHA_CHANNEL\n";
        if (m_showChannel == 5) m_debugDefines += "#define ENVIRONMENT_CHANNEL\n";
        if (m_showChannel == 6) m_debugDefines += "#define SHADOW_CHANNEL\n";
    }
}


// EffectManager.cpp:438. complete except the two A2M values.

void EffectManager::rebuildShaderDefines()
{
    RendDX9Base* base = reinterpret_cast<RendDX9Base*>(g_rendererBase);
    char line[128];

    m_shaderDefines = "";

    m_shaderDefines += isNvidia() ? "#define NVIDIA 1\n" : "#define NVIDIA 0\n";
    m_shaderDefines += isAti()    ? "#define ATI 1\n"    : "#define ATI 0\n";

    // A shader-model level requires support from both vertex and pixel shaders.
    m_shaderDefines += (deviceCapsPixelShaderMajor() < 3 || deviceCapsVertexShaderMajor() < 3)
        ? "#define SM30 0\n" : "#define SM30 1\n";
    m_shaderDefines += (deviceCapsPixelShaderMajor() < 2 || deviceCapsVertexShaderMajor() < 2)
        ? "#define SM20 0\n" : "#define SM20 1\n";

    // base[435] - byte 1740, the same field updateShaderDefines prints as rapath.
#if defined(_M_IX86)
    const unsigned int raPath =
        base ? *reinterpret_cast<const unsigned int*>(reinterpret_cast<const unsigned char*>(base) + 1740) : 0u;
#else
    // [CHOSEN, not recovered] (issue #7) The x64 Renderer is native, so the field (+0x6CC) is read
    // through its accessor, which needs the published Renderer. The first rebuild runs inside the
    // base construction (constructD3DHalf), before `g_Renderer = this`, when the Renderer
    // constructor has already zeroed the field - 0 is what the read would find there.
    const unsigned int raPath = (base && g_Renderer) ? rendererRaPath() : 0u;
#endif
    ::sprintf_s(line, "#define RAPATH %i \n", raPath);
    m_shaderDefines += line;

    // base[1302] gates it alongside the multisample check - see hasAlpha2Mask in ShaderManager.cpp.
    bool a2m = false;
    if (base)
    {
        // Unlike the shared probe, this test reads the first type unsigned (any non-zero type).
        const bool multisampleOk =
            !base->multiSamples.empty() ? (base->multiSamples[0].type > 0) : false;
        a2m = (base->forceMultisample                                  // base[1300]
               || (!base->disableMultisample && multisampleOk))        // base[1301]
              && base->hasAlphaToCoverage;                             // base[1302]
    }
    ::sprintf_s(line, "#define HASALPHA2MASK %i \n", a2m ? 1 : 0);
    m_shaderDefines += line;

    // "AdaptiveTess_Y" is nvidia's documented alpha-to-coverage trigger - a magic FOURCC written
    // into a render state. On anything else the define falls back to PointSize, which is the same
    // trick played through a different state.
    m_shaderDefines += isNvidia() ? "#define Alpha2Mask AdaptiveTess_Y \n"
                                  : "#define Alpha2Mask PointSize \n";

    // the shared transparency-AA codes, see transparencyAACode above.
    ::sprintf_s(line, "#define A2MEnable 0x%X \n", transparencyAACode(g_transparencyAAOn));
    m_shaderDefines += line;
    ::sprintf_s(line, "#define A2MDisable 0x%X \n", transparencyAACode(g_transparencyAAOff));
    m_shaderDefines += line;

    // [RENDER] Texture filtering is a shader contract: every material sampler states 16x
    // anisotropic and every data sampler states point or linear. The retail block that defined
    // FILTER_STM_* / FILTER_BM_* from VideoOptions quality 7 and the caps is gone.
}

namespace {

// PE +0x14 removes all listener registrations; +0x18 removes one named registration.
// MSVC reverses same-name virtual overloads, so declare the named overload first.
#if defined(_MSC_VER)
class IFileChangeManagerPe : public IBase
{
public:
    virtual renddx9abi::HostString addListener(IBase* listener,
                                               const renddx9abi::HostString& name,
                                               unsigned int userData) = 0; // +0x10
    virtual bool removeListener(IBase* listener,
                                const renddx9abi::HostString& name) = 0; // +0x18
    virtual bool removeListener(IBase* listener) = 0; // +0x14
};
#endif

} // anonymous namespace

namespace {

EffectSubObject* includeFromListener(void* listener)
{
    return reinterpret_cast<EffectSubObject*>(static_cast<unsigned char*>(listener)
        - offsetof(EffectSubObject, vptr2));
}

// PE 0x100359A0
EffectSubObject::~EffectSubObject()
{
    vptr = raIncludeVtable();
    vptr2 = raIncludeListenerVtable();
#if !defined(_MSC_VER)
    // [CHOSEN, not recovered] Use the native provider's overload ordering.
    static_cast<io::IFileChangeManager*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->removeListener(reinterpret_cast<io::IFileChangeListener*>(&vptr2));
#else
    static_cast<IFileChangeManagerPe*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->removeListener(reinterpret_cast<IBase*>(&vptr2));
#endif
}

// PE 0x1000CB20; the listener is the secondary face, with the count at its +4.
int RD9_THUNK includeListenerAddRef(void* listener RD9_THUNK_EDX)
{
    return ++includeFromListener(listener)->refCount;
}

// PE 0x101A9A10
int RD9_THUNK includeListenerGetRef(void* listener RD9_THUNK_EDX)
{
    return includeFromListener(listener)->refCount;
}

// PE 0x10037D60
int RD9_THUNK includeListenerRelease(void* listener RD9_THUNK_EDX)
{
    EffectSubObject* const sub = includeFromListener(listener);
    const int remaining = --sub->refCount;
    if (!remaining && sub)
    {
        sub->~EffectSubObject();
        ::renddx9abi::engineFree(sub);
    }
    return remaining;
}

// PE 0x10037420; IBase deliberately answers the include face, not this listener face.
void* RD9_THUNK includeListenerQuery(void* listener RD9_THUNK_EDX, unsigned int iid)
{
    EffectSubObject* const sub = includeFromListener(listener);
    if (iid == RD9_IID_IBASE) return sub;
    if (iid == 0x3C7B54FFu) return sub ? listener : 0; // IID_IFileChangeListener, dword_10208E10
    return 0;
}

// PE 0x10037D30; neither callback argument nor the receiver is read.
void RD9_THUNK includeListenerChanged(void* /*listener*/ RD9_THUNK_EDX,
                                     const std::string& /*name*/, unsigned int /*userData*/)
{
    static_cast<EffectManager*>(g_effectManager)->resetEffects();
}

void* const kRaIncludeListenerVtable[5] =
{
    reinterpret_cast<void*>(&includeListenerAddRef),
    reinterpret_cast<void*>(&includeListenerGetRef),
    reinterpret_cast<void*>(&includeListenerRelease),
    reinterpret_cast<void*>(&includeListenerQuery),
    reinterpret_cast<void*>(&includeListenerChanged)
};

void* raIncludeListenerVtable()
{
    return const_cast<void**>(kRaIncludeListenerVtable);
}

} // anonymous namespace

// PE 0x1001A020.
int EffectManager::release()
{
    const int remaining = --m_refCount;
    if (!remaining)
        reinterpret_cast<EffectManagerPrimaryFace*>(this)->destroy(1);
    return remaining;
}

// PE 0x10034170. MetaFx cache ownership uses direct destruction, not release().
void EffectManager::clearMetaFx()
{
    for (const auto& item : m_metaFxCache)
    {
        MetaFx* const meta = item.second;
        if (meta)
        {
            meta->~MetaFx();
            ::renddx9abi::engineFree(meta);
        }
    }
    m_metaFxCache.clear();
}

// PE 0x10035BC0.
EffectManager::~EffectManager()
{
    m_vptr = effectManagerPrimaryVtable();
    m_vptr2 = effectManagerListenerVtable();
#if !defined(_MSC_VER)
    static_cast<io::IFileChangeManager*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->removeListener(reinterpret_cast<io::IFileChangeListener*>(this));
#else
    static_cast<IFileChangeManagerPe*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->removeListener(reinterpret_cast<IBase*>(this));
#endif
    if (m_sub)
    {
        m_sub->~EffectSubObject();
        ::renddx9abi::engineFree(m_sub);
    }
    for (auto& item : m_effectCache)
    {
        static_cast<ID3DXEffect*>(item.second)->Release();
        item.second = 0;
    }
    clearMetaFx();
    // Native members then unwind memory files, define strings, caches and name.
}

// The reset is deliberately broad: shader/debug define strings, every material
// queue's compiled effect, every cached Effect wrapper/watch, and the secondary string cache.
void EffectManager::resetEffects()
{
    rebuildDebugDefines();
    rebuildShaderDefines();
    renderQueuesResetEffects();

    for (RaEffectCache::iterator node = m_effectCache.begin(); node != m_effectCache.end(); ++node)
    {
        if (node->second)
        {
            static_cast<ID3DXEffect*>(node->second)->Release();
            node->second = 0;
        }

        const renddx9abi::HostString hostKey = renddx9abi::makeHostString(node->first);
#if !defined(_MSC_VER)
        // [CHOSEN, not recovered] Use the provider's typed native interface.
        static_cast<io::IFileChangeManager*>(static_cast<void*>(::dice::hfe::fileChangeManager))
            ->removeListener(reinterpret_cast<io::IFileChangeListener*>(this), hostKey);
#else
        static_cast<IFileChangeManagerPe*>(
            static_cast<void*>(::dice::hfe::fileChangeManager))->removeListener(
                reinterpret_cast<IBase*>(this), hostKey);
#endif
    }

    // sub_100369E0 over the cache's nodes, then the head relinked.
    m_effectCache.clear();

    // sub_10036A80 over the memory files: each node's mapped string, then its key string.
    m_memoryFiles.clear();
}

namespace {


// Device loss rebuilds defines and drops compiled effects/watches; reset does nothing.
// Subsequent draws compile effects again, using valid disk caches when available.

void RD9_THUNK em_onDeviceLost(void* listener RD9_THUNK_EDX)
{
    EffectManager::fromDeviceLostListener(listener)->resetEffects();
}

void RD9_THUNK em_listenerNullsub(void* RD9_THUNK_EDX)
{
    // 0x10037D40 - `ret`, empty in the original too.
}

void* const kEffectManagerListenerVtable[3] =
{
    reinterpret_cast<void*>(&em_onDeviceLost),      // +0x00
    reinterpret_cast<void*>(&em_listenerNullsub),   // +0x04
    reinterpret_cast<void*>(&em_listenerNullsub),   // +0x08
};

void* effectManagerListenerVtable()
{
    return const_cast<void**>(kEffectManagerListenerVtable);
}

} // anonymous namespace

// PE 0x10034430.
void EffectManager::setShowChannel(unsigned int channel)
{
    if (m_showChannel != channel)
    {
        m_showChannel = channel;
        resetEffects();
    }
}


// The MetaFx cache at +0x54: lowered .mfx name -> MetaFx, case-sensitive (strcmp) order.


namespace {


// Compatibility FileInfo needs a live VC7.1 string and the full 64-byte output record,
// even though this path only consumes getFileInfo's Boolean result.


bool fileManagerGetFileInfo(const std::string& path)
{
    // [CHOSEN, not recovered] Permit loading before the host publishes FileManager.
    if (!::dice::hfe::fileManager)
        return false;

    const renddx9abi::HostString hostPath = renddx9abi::makeHostString(path);

    ::dice::hfe::io::FileInfo info;
    return ::dice::hfe::fileManager->getFileInfo(hostPath, info);
}


// Prefixes unqualified shader paths; a null renderer selects xpack.
// [CHOSEN, not recovered] Scratch strings are local rather than shared static storage.

void applyModPrefix(std::string& path)
{
    if (::strstr(path.c_str(), "mods/"))
        return;

    bool xpack = true;                 // the shipped default when g_Renderer is null
    if (g_Renderer)
    {
        xpack = static_cast<IRenderer*>(g_Renderer)->isExpansionPack();
    }

    std::string prefixed(xpack ? "mods/xpack/" : "mods/bf2/");
    prefixed += path;
    path = prefixed;
}

} // anonymous namespace


// Returns a cached MetaFx by lower-case key, loading with the original name on a miss.
// PE 0x10033D40 (listing read).
MetaFx* EffectManager::getMetaFx(const std::string& name)
{
    // sub_10074E80 is `std::string toLower(const std::string&)` - a copy, then the in-place
    // lowercase at sub_10074D10 that this tree already carries as pathToLower.
    std::string key(name);
    pathToLower(key);

    // sub_10036410 is std::map::find: a lower_bound (sub_101C0AD0) and then "is the probe less
    // than what we landed on", which selects between the node and end(). Its caller compares the
    // result against *(this + 0x58) - the head - so a miss is end().
    const RaMetaFxCache::const_iterator node = m_metaFxCache.find(key);
    if (node != m_metaFxCache.end())
        return node->second;

    // Requires the render-queue table published by initializeD3DSubsystems: its +0xB4 byte.
    const unsigned char flag = static_cast<const RaShaderTable*>(g_renderQueues)->byteB4;

    return loadMetaFx(name, flag != 0);
}


// Constructs and caches a MetaFx; returns null if it is already cached.
// Use getMetaFx to retrieve existing entries. Diagnostics use the resolved path.
// PE 0x10033B10 (listing read).
MetaFx* EffectManager::loadMetaFx(std::string name, bool flag)
{
    {
        // The re-check, on the same lowered key and the same map as getMetaFx.
        std::string key(name);
        pathToLower(key);
        if (m_metaFxCache.find(key) != m_metaFxCache.end())
            return 0;                       // null, NOT the cached MetaFx - see above
    }

    // THE fixup IS guarded ON g_Renderer BY THE caller even though sub_10029D00 handles a null
    // g_Renderer itself. Both tests are shipped; both are kept.
    std::string path(name);
    if (g_Renderer)
        applyModPrefix(path);

    if (!fileManagerGetFileInfo(path))      // FileManager vslot +0x38
    {
        ::renddx9abi::logf("EffectManager.cpp:483: Can't find metaShader file: %s", path.c_str());
        return 0;
    }

    MetaFx* metaFx = 0;

    // sub_1005F320(0xC0) in the image; see metaFxObjectSize's note on why the literal is not used.
    void* const storage = ::renddx9abi::engineAlloc(metaFxObjectSize());
    if (storage)
    {
        // Derive extension before stem from the resolved path.
        std::string extension;
        extension = extractFileExt(path);                   // sub_10075180 -> sub_10075580
        std::string stem;
        pathStripExtension(stem, path);                      // sub_100753D0 -> sub_10075A50

        // AND THE boolean IS inverted ON THE WAY IN. 0x10033C4E is `cmp [ebp+arg_1C], 0` -
        // a byte compare - followed by `setz al`, so the constructor receives the negation of the
        // byte getMetaFx read out of the render-queue table.
        metaFx = metaFxConstruct(storage, stem, extension, !flag);
    }
    // NO free() ON THE failure ARM, because there is no failure arm: a null allocation skips the
    // constructor entirely (0x10033C4C) and the two strings are never built, which is what the
    // `ebx = 3` / `ebx = var_4` destructor-flag bookkeeping at 0x10033C5B and 0x10033C7E encodes.

    if (metaFx)
    {
        // sub_10074E80 a second time - the first lowered key was destroyed with the re-check.
        std::string key(name);
        pathToLower(key);

        // sub_100367F0 - std::map::insert(value_type(key, metaFx)). The pair is assembled on the
        // stack at 0x10033CB5 (the string) and 0x10033CC1 (the pointer 28 bytes later), copied
        // once more at 0x10033CD4, and handed over by reference. The key cannot already be present
        // here: the re-check above returned on a hit.
        m_metaFxCache.insert(std::make_pair(key, metaFx));

        metaFxParse(metaFx);
    }

    return metaFx;
}

// dword_1023D954 in the shipped image.
void* g_effectManager = 0;

bool createEffectManager(IDirect3DDevice9* device, bool disableShaderCache)
{
    EffectManager* em = new EffectManager(device, disableShaderCache);
    g_effectManager = em;

    ::renddx9abi::logf("EffectManager: built at 0x%08X, %u bytes of shader defines",
                       ::renddx9abi::addr(em), em->shaderDefines().size());
    return true;
}

// the expression initializeD3DSubsystems registers at 0x1000B3DD..0x1000B3FB:
// `dword_1023D954 ? dword_1023D954 + 4 : 0`. The null arm is shipped - it registers a null listener.
void* effectManagerDeviceLostListener()
{
    return g_effectManager ? static_cast<EffectManager*>(g_effectManager)->deviceLostListener() : 0;
}

// [CHOSEN, not recovered] Owner-TU seam for PE 0x1000AE7D..0x1000AE8E.
// The caller removes the listener first; retain the primary deleting-slot dispatch.
void destroyEffectManager()
{
    if (g_effectManager)
        static_cast<EffectManagerPrimaryFace*>(g_effectManager)->destroy(1u);
}

// The +0x40 byte (the _FORCE_1_4_SHADERS_ input) for ShaderManager.cpp and RaShaderPermutator.cpp,
// and its writer for VideoOptions' shader-path change. Unguarded, as the shipped readers are.
unsigned char effectManagerFlag40()
{
    return static_cast<EffectManager*>(g_effectManager)->flag40() ? 1u : 0u;
}

void effectManagerSetFlag40(unsigned char value)
{
    static_cast<EffectManager*>(g_effectManager)->setFlag40(value);
}

} // namespace Ra

void effectManagerSetShowChannel(unsigned int channel)
{
    // PE sub_10034430 dereferences the singleton without a guard.
    static_cast<Ra::EffectManager*>(Ra::g_effectManager)->setShowChannel(channel);
}

// sub_10033240 for VideoOptions' resolution change (sub_1000F8D0, its last call:
// 0x1000FB54 `mov ecx, dword_1023D954` / 0x1000FB5A `call sub_10033240`). Unguarded, as that call
// site is. It runs right after the two probes, so the A2M defines pick up what they just found.
void effectManagerRebuildShaderDefines()
{
    static_cast<Ra::EffectManager*>(Ra::g_effectManager)->rebuildShaderDefines();
}

// The seam StaticMeshStateManager::initTechniques (Code/BF2/Geom/StaticMeshTemplate.cpp, the
// RW-124 frontier) needs. Its declaration belongs beside effectManagerSetShowChannel in
// RendDX9Base.h and is deliberately NOT added there this sitting, because another agent owns that
// header right now; whoever unblocks the Geom side adds the one line.

// unguarded ON purpose. Both shipped call sites - 0x10115D08 and load
// dword_1023D954 straight into ecx and call, with no null test.
Ra::MetaFx* effectManagerGetMetaFx(const std::string& name)
{
    return static_cast<Ra::EffectManager*>(Ra::g_effectManager)->getMetaFx(name);
}


// Returns the compiled ID3DXEffect for path, or null. Requires the effect-manager singleton.

void* effectManagerGetEffect(const std::string& path)
{
    return static_cast<Ra::EffectManager*>(Ra::g_effectManager)->getEffect(path);
}

// [TOOLS] tools.survey (ToolsSurvey.cpp): every compiled Ra effect and its cache name.
void effectManagerForEachEffect(void (*visit)(const std::string& name, void* d3dxEffect, void* context),
                                void* context)
{
    Ra::EffectManager* const manager = static_cast<Ra::EffectManager*>(Ra::g_effectManager);
    if (!manager)
        return;
    const Ra::RaEffectCache& cache = manager->effectCache();
    for (Ra::RaEffectCache::const_iterator node = cache.begin(); node != cache.end(); ++node)
        if (node->second)
            visit(node->first, node->second, context);
}


// In-memory generated shader files: a string -> string map at +0x9C (72-byte nodes in the image).
// Names compare case-insensitively and are also lowercased.

namespace Ra {


// Inserts a lowercase name without replacing duplicates; always returns true.

// PE 0x10034080.
bool EffectManager::addMemoryFile(const std::string& name, const std::string& contents)
{
    std::string lowered(name);
    pathToLower(lowered);

    // std::map::insert semantics: an existing equivalent key wins and nothing is stored.
    m_memoryFiles.insert(std::make_pair(lowered, contents));
    return true;                                      // shipped: the result is a constant true
}


// 0x10034460, 79 B, `retn 4`. `erase(lower_bound(k), upper_bound(k))` - and note the shipped order:
// upper_bound is computed first (0x10034481) and lower_bound second (0x1003448B), which is
// equal_range evaluated backwards. Preserved, because both descents read the same tree and the
// order is observable if either ever faulted.

// PE 0x10034460.
void EffectManager::removeMemoryFile(const std::string& name)
{
    std::string lowered(name);
    pathToLower(lowered);                          // sub_10074E80's lowering half

    const RaMemoryFileMap::iterator last  = m_memoryFiles.upper_bound(lowered);   // sub_10035FD0, first
    const RaMemoryFileMap::iterator first = m_memoryFiles.lower_bound(lowered);   // sub_10035E40, second
    m_memoryFiles.erase(first, last);
}


// Ra effects use virtual include files, a per-adapter compiled cache and a shader-model
// search. Their include resolver also maintains the fully expanded source text.


namespace {


// FileManager vslot +0x38 again, but this time the record is read: the size and the last-write
// filetime, which is what the .cfx freshness test compares. The 64-byte PE record is the one the
// block above fileManagerGetFileInfo describes; the native flavour has the typed io::FileInfo, and
// the two are NOT the same layout, which is why this is written once, here, with the split.

struct RaFileInfo
{
    unsigned int size;
    unsigned int writeTimeLow;
    unsigned int writeTimeHigh;
};

bool raGetFileInfo(const std::string& path, RaFileInfo& out)
{
    out.size = 0;
    out.writeTimeLow = 0;
    out.writeTimeHigh = 0;

    // [CHOSEN, not recovered] Permit loading before the host publishes FileManager.
    if (!::dice::hfe::fileManager)
        return false;

    const renddx9abi::HostString hostPath = renddx9abi::makeHostString(path);

    ::dice::hfe::loadtrace::Scope statTrace("fx.stat", path.c_str());   // [DEV] BF2_LOAD_TRACE
    ::dice::hfe::io::FileInfo info;
    if (!::dice::hfe::fileManager->getFileInfo(hostPath, info))
        return false;
    out.size          = info.size;
    out.writeTimeLow  = info.lastWriteTimeLow;
    out.writeTimeHigh = info.lastWriteTimeHigh;
    return true;
}

// FileManager vslot +0x18. The read pair is (2, 0) and the write pair is (0, 2), both read off
// their call sites: 0x1003528C pushes 0,0,2,path and 0x1003317F pushes 0,2,0,path.
renddx9abi::HostStream* raOpen(const std::string& path, int mode, int access)
{
    if (!::dice::hfe::fileManager)
        return 0;
    const renddx9abi::HostString hostPath = renddx9abi::makeHostString(path);
    return ::dice::hfe::fileManager->open(hostPath,
                                          static_cast<renddx9abi::HostFileMode>(mode),
                                          static_cast<renddx9abi::HostFileAccess>(access), 0);
}

// Returns the basename without its final extension, accepting either path separator.
void raCacheStem(std::string& out, const std::string& in)
{
    std::string t(in.c_str(), in.size());
    const std::string::size_type back = t.rfind('\\');
    const std::string::size_type fwd  = t.rfind('/');
    std::string::size_type cut = back;
    if (fwd != std::string::npos && (cut == std::string::npos || fwd > cut))
        cut = fwd;
    if (cut != std::string::npos)
        t = t.substr(cut + 1);
    const std::string::size_type dot = t.rfind('.');
    if (dot != std::string::npos)
        t = t.substr(0, dot);
    out = t.c_str();
}

// Generates the first permutation whose cleaned name matches wanted; returns false on a miss.
// PE 0x1003DD70.
bool raGenerateShaderByCleanName(MetaFx* fx, const std::string& wanted)
{
    std::string clean;
    clean.reserve(256);
    const MetaFxStringArray& names = fx->getGeneratedNames();
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        clean = cleanPermutationName(names[i]);   // PE 0x1003C440
        if (clean == wanted)
        {
            fx->generateShader(names[i]);
            return true;
        }
    }
    return false;
}

} // anonymous namespace


// Regenerates cached in-memory shaders only when the memory store is empty.
// The mode test assumes every MetaFx was constructed by loadMetaFx with the queue flag;
// a second producer requires reading each object's own mode instead.

void EffectManager::refillMemoryFiles()
{
    if (!m_memoryFiles.empty())
        return;

    const bool writeGeneratedFiles =
        static_cast<const RaShaderTable*>(g_renderQueues)->byteB4 == 0;   // the table's +0xB4

    for (RaMetaFxCache::const_iterator node = m_metaFxCache.begin();
         node != m_metaFxCache.end(); ++node)
    {
        MetaFx* const fx = node->second;
        if (!fx)
            continue;
        if (!writeGeneratedFiles)
            metaFxParse(fx);
    }
}


// sub_10033AA0, 0x10033AA0, 101 B, `retn 4`. The in-memory shader-file lookup.

//     refillMemoryFiles();
//     it = m_memoryFiles.find(toLower(name));
//     return it == end() ? 0 : &it->second;

// Refreshes memory files before looking up the lowercase name.

const std::string* EffectManager::findMemoryFile(const std::string& name)
{
    refillMemoryFiles();

    std::string key(name);
    pathToLower(key);

    // sub_10036470 - the map's find, the same `_stricmp` lower_bound shape as sub_10036390.
    const RaMemoryFileMap::const_iterator node = m_memoryFiles.find(key);
    if (node == m_memoryFiles.end())
        return 0;
    return &node->second;
}


// Generates a missing shader from the first matching MetaFx name.
// The suffix starts at prefix length even when the substring match was not at offset zero.

bool EffectManager::generateMissingShader(const std::string& name)
{
    std::string stripped;
    pathStripExtension(stripped, name);
    std::string wanted(stripped);
    pathToLower(wanted);                                      // the sub_10074E80 half

    for (RaMetaFxCache::const_iterator node = m_metaFxCache.begin();
         node != m_metaFxCache.end(); ++node)
    {
        std::string prefix;
        pathStripExtension(prefix, node->first);

        const std::string haystack(wanted.c_str(), wanted.size());
        if (haystack.find(prefix.c_str(), 0, prefix.size()) == std::string::npos)
            continue;

        const std::string tail(wanted.c_str() + prefix.size());
        MetaFx* const fx = node->second;
        raGenerateShaderByCleanName(fx, tail);
        return true;
    }
    return false;
}


// Reads a NUL-terminated effect source into engine-allocated bufferOut and returns its size.
// Tries disk, generated memory files, then on-demand generation. Returns zero on failure;
// buffers allocated before a read/open failure are retained.

unsigned int EffectManager::readEffectFile(const std::string& name, unsigned char** bufferOut)
{
    ::dice::hfe::loadtrace::Scope readTrace("fx.read", name.c_str());   // [DEV] BF2_LOAD_TRACE
    std::string path(name);
    if (g_Renderer)
        applyModPrefix(path);

    RaFileInfo info;
    if (raGetFileInfo(path, info))                            // FileManager +0x38
    {
        *bufferOut = static_cast<unsigned char*>(::renddx9abi::engineAlloc(info.size + 1));

        renddx9abi::HostStream* const file = raOpen(path, 2, 0);   // FileManager +0x18
        if (!file)
        {
            ::renddx9abi::logf("EffectManager.cpp:884: Can't open effect file: %s", path.c_str());
            return 0;
        }

        // THE `*bufferOut ?` test IS ours. sub_1005F340's answer is used unchecked at
        // a failed allocation there hands the host FileManager a null destination.
        const unsigned int read = *bufferOut ? file->read(*bufferOut, info.size) : 0;
        file->release();

        if (read != info.size)
        {
            ::renddx9abi::logf("EffectManager.cpp:892: Can't read whole file: %s", path.c_str());
            return 0;
        }

        (*bufferOut)[read] = 0;
        return read;
    }

    const std::string* const stored = findMemoryFile(path);
    if (stored)
    {
        // An effect source; its size fits the shipped 32-bit count.
        const unsigned int size = static_cast<unsigned int>(stored->size());
        *bufferOut = static_cast<unsigned char*>(::renddx9abi::engineAlloc(size + 1));
        if (*bufferOut)
            ::strcpy(reinterpret_cast<char*>(*bufferOut), stored->c_str());
        return size;
    }

    if (generateMissingShader(name))
        return readEffectFile(name, bufferOut);               // the shipped recursion

    ::renddx9abi::logf("EffectManager.cpp:871: Can't find effect file: %s Loading Filename:%s",
                       name.c_str(), path.c_str());
    return 0;
}


// PE 0x10033860 (listing read). Unreachable in 1.50: no code or data references. For every cached
// effect whose name contains `filter` (case-insensitive) it disassembles the effect with colour code
// and writes the text to "<name without extension>_dis.htm" through the file manager. Always returns
// true. Retail continues into the write after an open failure, dereferencing the null stream and the
// buffer it has just released; that continuation is omitted here and the effect is skipped.
bool EffectManager::dumpEffectDisassembly(const std::string& filter)
{
#if defined(_WIN32)
    for (RaEffectCache::iterator node = m_effectCache.begin(); node != m_effectCache.end(); ++node)
    {
        // PE 0x10075110: both names lowered, then std::string::find on the pair.
        std::string loweredName(node->first.c_str());
        std::string loweredFilter(filter.c_str());
        pathToLower(loweredName);
        pathToLower(loweredFilter);
        if (loweredName.find(loweredFilter.c_str(), 0, loweredFilter.size()) == std::string::npos)
            continue;

        ID3DXBuffer* buffer = 0;
        const long hr = D3DXDisassembleEffect(static_cast<ID3DXEffect*>(node->second), TRUE, &buffer);
        if (hr != 0)
        {
            ::renddx9abi::logf("Ra/EffectManager.cpp:1469 Failed to disasm. %s",
                               DXGetErrorDescription9A(hr));
            continue;
        }

        std::string stem;
        pathStripExtension(stem, node->first);
        std::string path(stem);
        path += "_dis.htm";
        renddx9abi::HostStream* const file = raOpen(path, 0, 2);   // FileManager +0x18, the write pair
        if (!file)
        {
            buffer->Release();
            ::renddx9abi::logf("Ra/EffectManager.cpp:1478 Failed to open disasm file.");
            continue;
        }

        file->write(buffer->GetBufferPointer(), buffer->GetBufferSize());   // stream +0x14
        file->release();
        buffer->Release();
    }
    return true;
#else
    // [CHOSEN, not recovered] The native D3DX backend has no effect disassembler; report and refuse.
    (void)filter;
    ::fprintf(stderr, "[CHOSEN, not recovered] dice::hfe::dx9rend::Ra::EffectManager::dumpEffectDisassembly: "
                      "no effect disassembler on this port\n");
    ::fflush(stderr);
    return false;
#endif
}

// Writes a binary compiled-effect cache with each byte XORed by its index.
// Returns false if the output cannot be opened; individual writes are unchecked.

bool EffectManager::writeEffectCache(ID3DXBuffer* blob, const std::string& path)
{
    renddx9abi::HostStream* const file = raOpen(path, 0, 2);  // FileManager +0x18, the write pair
    if (!file)
        return false;

    const unsigned int size = blob->GetBufferSize();

    char* const copy = static_cast<char*>(::renddx9abi::engineAlloc(size));
    if (!copy)
    {
        // NOT shipped. sub_1005F340's answer is used unchecked at 0x100331AC; this build refuses
        // rather than memcpy through a null, and says so, because a failed cache write costs one
        // recompile next run and nothing else.
        ::renddx9abi::logf("EffectManager::writeEffectCache: %u-byte engine allocation failed for "
                           "\"%s\"; the cache file is not written", size, path.c_str());
        file->release();
        return false;
    }

    const char* const src = static_cast<const char*>(blob->GetBufferPointer());
    ::memcpy(copy, src, size);

    for (unsigned int i = 0; i < size; ++i)
        copy[i] = static_cast<char>(copy[i] ^ static_cast<char>(i));

    file->write(copy, size);                                  // stream +0x14
    file->release();                                          // stream +0x08
    ::renddx9abi::engineFree(copy);
    return true;
}

// The two seams Ra/RaShaderPermutator.cpp calls. The shipped bodies are methods on the
// EffectManager; the permutator reaches them through the singleton, exactly as its own call sites
// do (`mov ecx, dword_1023D954`). Unguarded, as those call sites are.
bool effectManagerAddMemoryFile(const std::string& name, const std::string& contents)
{
    return static_cast<EffectManager*>(g_effectManager)->addMemoryFile(name, contents);
}

// sub_10034210 - published for Renderer::handleLevelPreLoad, which calls it directly one
// statement before the load pass (which calls it again; both are shipped).
void effectManagerResetEffects()
{
    static_cast<EffectManager*>(g_effectManager)->resetEffects();
}

void effectManagerRemoveMemoryFile(const std::string& name)
{
    static_cast<EffectManager*>(g_effectManager)->removeMemoryFile(name);
}

// byte_1023D950 - set to 1 the first time a compile produces a usable technique (PE 0x100357B9).
// The same retail byte is read and written by SwiffRender's getFlag/setFlag slots (PE 0x10153490 /
// PE 0x101534A0), so it has external linkage and SwiffRender.cpp refers to this one definition.
unsigned char g_anyEffectCompiled = 0;

namespace {


// Default compile flags disable preshaders; debug, skipped optimization and partial precision
// remain off unless their switches change.

unsigned char g_noPreshader      = 1;   // byte_102382A0 - ONE, not zero
unsigned char g_shaderDebug      = 0;   // byte_1023DD9B
unsigned char g_skipOptimization = 0;   // byte_1023DD9A
unsigned char g_partialPrecision = 0;   // byte_1023DD99
unsigned char g_forceOneFour     = 0;   // byte_1023DD98


// The macro tables, transcribed from 0x101E1268..0x101E1308. Each row is {name, value}.
const char* const kPsVersion[5][2] =
{
    { "PSVERSION", "13" }, { "PSVERSION", "14" }, { "PSVERSION", "21" },
    { "PSVERSION", "22" }, { "PSVERSION", "20" }
};
const char* const kPsModel[5][2] =
{
    { "PSMODEL", "ps_1_3" }, { "PSMODEL", "ps_1_4" }, { "PSMODEL", "ps_2_a" },
    { "PSMODEL", "ps_2_b" }, { "PSMODEL", "ps_2_0" }
};
const char* const kVsModel[2][2] =
{
    { "VSMODEL", "vs_1_1" }, { "VSMODEL", "vs_2_0" }
};
const char* const kForce14[2][2] =
{
    { "_FORCE_1_4_SHADERS_", "0" }, { "_FORCE_1_4_SHADERS_", "1" }
};
// dword_101E12F8 - the minimum (major<<8 | minor) pixel-shader version each psmodel row needs.
const unsigned int kMinPsVersion[5] = { 0x103u, 0x104u, 0x201u, 0x202u, 0x200u };
// The defines fillProfileDefines writes before its terminator: retail's six and [RENDER] RA_CONTRACT.
const int kRaProfileDefines = 7;

// PE 0x10035DC0.
template <typename Interface>
inline void raRelease(Interface*& p)
{
    // sub_10035DC0 - `if (p) { p->Release(); p = 0; }`, the 23-byte helper every arm here uses.
    if (p)
    {
        p->Release();
        p = 0;
    }
}

// [CHOSEN, not recovered] Native regex carrier for the fixed PME expressions below (issue #7).
// PCRegExp.dll is a VC7.1/x86 dependency. Retain its global cursor and input-object identity;
// C++14 ECMAScript has no multiline/dotall options, and its dot also excludes carriage returns.
class RaProjectedPattern
{
public:
    RaProjectedPattern(const std::string& expression, bool dotAll = false)
        : anchored(!expression.empty() && expression[0] == '^'), lastInput(0), cursor(0)
    {
        std::string native;
        bool characterClass = false;
        for (std::string::size_type i = anchored ? 1 : 0; i < expression.size(); ++i)
        {
            const char c = expression[i];
            if (c == '\\' && i + 1 < expression.size())
            {
                native += c;
                native += expression[++i];
                continue;
            }
            if (c == '[') characterClass = true;
            if (c == ']') characterClass = false;
            if (c == '.' && !characterClass)
                native += dotAll ? "[\\s\\S]" : "[^\\n]";
            else
            {
                // The only braces in these fixed expressions delimit an asm block.
                if (!characterClass && (c == '{' || c == '}')) native += '\\';
                native += c;
            }
        }
        pattern.assign(native, std::regex::ECMAScript | std::regex::icase);
    }

    void reset()
    {
        lastInput = 0;
        cursor = 0;
        markers.clear();
        subject.clear();
    }

    bool match(const std::string& input)
    {
        if (lastInput != &input)
            cursor = 0;
        lastInput = &input;
        markers.clear();
        subject = input;
        std::string::size_type at = cursor;
        if (anchored && at && at <= subject.size() && subject[at - 1] != '\n')
        {
            at = subject.find('\n', at);
            if (at != std::string::npos) ++at;
        }
        while (at != std::string::npos && at <= subject.size())
        {
            std::cmatch found;
            const char* const begin = subject.c_str() + at;
            const bool matched = std::regex_search(begin, subject.c_str() + subject.size(), found,
                pattern, anchored ? std::regex_constants::match_continuous
                                  : std::regex_constants::match_default);
            if (matched)
            {
                for (size_t i = 0; i < found.size(); ++i)
                {
                    const std::string::size_type first = at + found.position(i);
                    markers.push_back(std::make_pair(first, first + found.length(i)));
                }
                cursor = markers[0].second;
                return true;
            }
            if (!anchored) break;
            at = subject.find('\n', at);
            if (at != std::string::npos) ++at;
        }
        cursor = 0;
        return false;
    }

    std::string group(unsigned int index) const
    {
        const std::pair<size_t, size_t>& span = markers[index];
        return subject.substr(span.first, span.second - span.first);
    }

    std::pair<size_t, size_t> span(unsigned int index) const { return markers[index]; }

private:
    bool anchored;
    std::regex pattern;
    const std::string* lastInput;
    std::string::size_type cursor;
    std::string subject;
    std::vector<std::pair<size_t, size_t> > markers;
};

// Pumps the renderer's inactive/device-lost wait and reports whether it reset the device.
// g_Renderer must be non-null. A true result invalidates shader compilation state.
bool rendererDeviceWasReset()
{
    // `mov ecx, dword_1023D098; mov eax, [ecx]; call [eax+0BCh]; test al, al; jne skip` - the
    // caller already tested g_Renderer; the dispatch is typed through the shared interface, whose
    // +0xBC is Renderer::waitWhileAppInactive (0x100055B0). true means "no wait, no reset".
    return !static_cast<IRenderer*>(g_Renderer)->waitWhileAppInactive();
}


// Lazily loads D3DXCreateEffectCompiler from d3dx9_24.dll.
// This local copy retains its module reference. Consolidate it with the permutator's
// helper when that helper has a shared declaration and external linkage.

#if defined(__APPLE__)
// [CHOSEN, not recovered] Native compiler entry; no Win32 DLL recursion workaround.
long raCreateEffectCompiler(const char* srcData, unsigned int srcDataLen,
    const D3DXMACRO* defines, LPD3DXINCLUDE include, unsigned long flags,
    ID3DXEffectCompiler** compilerOut, ID3DXBuffer** parseErrorsOut)
{
    return D3DXCreateEffectCompiler(srcData, srcDataLen, defines, include,
        static_cast<DWORD>(flags), compilerOut, parseErrorsOut);
}
#else
typedef HRESULT (WINAPI* RaD3dxCreateEffectCompilerFn)(LPCSTR, UINT, CONST D3DXMACRO*,
                                                       LPD3DXINCLUDE, DWORD,
                                                       LPD3DXEFFECTCOMPILER*, LPD3DXBUFFER*);

HMODULE                      s_raD3dxModule = 0;              // dword_1023E108
RaD3dxCreateEffectCompilerFn s_raCreateEffectCompiler = 0;    // dword_1023E10C

// [PERF] The lazy load on its own, so the effect prefetch can bind the entry point on the main
// thread before its workers call raCreateEffectCompiler.
bool raLoadEffectCompiler()
{
    if (!s_raD3dxModule)
    {
        s_raD3dxModule = ::LoadLibraryA("d3dx9_24.dll");
        if (s_raD3dxModule)
        {
            s_raCreateEffectCompiler = reinterpret_cast<RaD3dxCreateEffectCompilerFn>(
                ::GetProcAddress(s_raD3dxModule, "D3DXCreateEffectCompiler"));
        }
        if (!s_raD3dxModule || !s_raCreateEffectCompiler)
        {
            // The diagnostic retains its original DLL-name typo.
            ::renddx9abi::logf("RaShaderPermutator.cpp:574: Failed to load D3DX9d_24.dll, we need "
                               "this to avoid the April 2005 DxSDK bug(recursive call on parsing "
                               "some .fx files (skinnedmesh.fx)");
            return false;
        }
    }
    return s_raCreateEffectCompiler != 0;
}

long raCreateEffectCompiler(const char* srcData, unsigned int srcDataLen,
                            const D3DXMACRO* defines, LPD3DXINCLUDE include, unsigned long flags,
                            ID3DXEffectCompiler** compilerOut, ID3DXBuffer** parseErrorsOut)
{
    if (!raLoadEffectCompiler())
        return static_cast<long>(0x80004005);                 // E_FAIL, as the shipped body returns

    return s_raCreateEffectCompiler(srcData, srcDataLen, defines, include, flags,
                                    compilerOut, parseErrorsOut);
}
#endif


// [FIX] The content-keyed effect cache (see compileEffect).
// raEffectContentKey: FNV-1a over the effect expanded by D3DXPreprocessShader (the same include
// handler and defines the compile uses, so every included file's text is in the hash), then the
// define list and the compile flags, as 16 hex digits. Empty when the source does not preprocess;
// the compile then reports the error and nothing is cached under a key.
namespace {

unsigned long long raFnv1a(unsigned long long hash, const void* data, std::size_t size)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

#if defined(__APPLE__)
// [PLATFORM] The native D3DX backend has no preprocessor, so there is no content key and the Ra
// effect cache is not used on macOS: every effect compiles.
long raPreprocessShader(const char*, unsigned int, const D3DXMACRO*, LPD3DXINCLUDE,
                        ID3DXBuffer**, ID3DXBuffer**)
{
    static bool reported = false;
    if (!reported)
    {
        reported = true;
        ::fprintf(stderr, "[EFFECTCACHE] Ra effect cache disabled: no D3DXPreprocessShader\n");
        ::fflush(stderr);
    }
    return static_cast<long>(0x80004001);                     // E_NOTIMPL
}
#else
typedef HRESULT (WINAPI* RaD3dxPreprocessShaderFn)(LPCSTR, UINT, CONST D3DXMACRO*, LPD3DXINCLUDE,
                                                   LPD3DXBUFFER*, LPD3DXBUFFER*);
// d3dcompiler's D3DPreprocess. D3D_SHADER_MACRO, ID3DInclude and ID3DBlob have the layouts of
// D3DXMACRO, ID3DXInclude and ID3DXBuffer, so the D3DX types are passed through.
typedef HRESULT (WINAPI* RaD3dPreprocessFn)(LPCVOID, SIZE_T, LPCSTR, CONST D3DXMACRO*, LPD3DXINCLUDE,
                                            LPD3DXBUFFER*, LPD3DXBUFFER*);

long raPreprocessShader(const char* srcData, unsigned int srcDataLen, const D3DXMACRO* defines,
                        LPD3DXINCLUDE include, ID3DXBuffer** textOut, ID3DXBuffer** errorsOut)
{
    // The compiler's module: D3DXPreprocessShader must expand exactly as its compiler does.
    // Its own handle: sharing s_raD3dxModule made raCreateEffectCompiler skip resolving its entry
    // point (it resolves only when it loads the module itself) and call null.
    // [FIX] Wine's d3dx9_24 (like _25 and _26) does not export D3DXPreprocessShader, so the key
    // was never computed and the cache never used under Wine. There it falls back to
    // d3dcompiler_47's D3DPreprocess: the key only has to be a deterministic expansion of the
    // effect through the same include handler and defines.
    static RaD3dxPreprocessShaderFn d3dxPreprocess = 0;
    static RaD3dPreprocessFn        d3dPreprocess = 0;
    static bool                     resolved = false;
    if (!resolved)
    {
        resolved = true;
        if (const HMODULE module = ::LoadLibraryA("d3dx9_24.dll"))
            d3dxPreprocess = reinterpret_cast<RaD3dxPreprocessShaderFn>(
                ::GetProcAddress(module, "D3DXPreprocessShader"));
        if (!d3dxPreprocess)
            if (const HMODULE module = ::LoadLibraryA("d3dcompiler_47.dll"))
                d3dPreprocess = reinterpret_cast<RaD3dPreprocessFn>(
                    ::GetProcAddress(module, "D3DPreprocess"));
    }
    if (d3dxPreprocess)
        return d3dxPreprocess(srcData, srcDataLen, defines, include, textOut, errorsOut);
    if (d3dPreprocess)
        return d3dPreprocess(srcData, srcDataLen, 0, defines, include, textOut, errorsOut);
    return static_cast<long>(0x80004005);                     // E_FAIL
}
#endif

} // anonymous namespace

std::string raEffectContentKey(const char* source, unsigned int sourceLen, const D3DXMACRO* defines,
                               LPD3DXINCLUDE include, unsigned long flags)
{
    ID3DXBuffer* text = 0;
    ID3DXBuffer* errors = 0;
    const long hr = raPreprocessShader(source, sourceLen, defines, include, &text, &errors);
    // A failure here silently disables the cache for the effect, so report the first one.
    static bool reported = false;
    if ((hr != 0 || !text) && !reported)
    {
        reported = true;
        const char* message = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "";
        ::fprintf(stderr, "[EFFECTCACHE] no cache key: D3DXPreprocessShader hr=0x%08lx text=%p: %.400s\n",
                  static_cast<unsigned long>(hr), static_cast<void*>(text), message);
        ::fflush(stderr);
    }
    if (errors)
        errors->Release();
    if (hr != 0 || !text)
    {
        if (text)
            text->Release();
        return std::string();
    }

    unsigned long long hash = 14695981039346656037ull;
    hash = raFnv1a(hash, text->GetBufferPointer(), text->GetBufferSize());
    text->Release();
    for (const D3DXMACRO* define = defines; define && define->Name; ++define)
    {
        hash = raFnv1a(hash, define->Name, ::strlen(define->Name) + 1);
        if (define->Definition)
            hash = raFnv1a(hash, define->Definition, ::strlen(define->Definition) + 1);
    }
    const unsigned int flagWord = static_cast<unsigned int>(flags);
    hash = raFnv1a(hash, &flagWord, sizeof(flagWord));

    char hex[17];
    ::sprintf_s(hex, "%016llx", hash);
    return std::string(hex);
}

// [PERF] raEffectSourceKey: the content key without the preprocessor. raEffectContentKey expands
// the whole effect for every key, about 10 ms each under Wine, and its include handler re-reads
// every header on every #include. This key hashes the source and each file reachable through its
// #include lines instead, each once, whether or not a conditional skips the include: any change
// to anything the preprocessor could read changes the key. Files resolve as raIncludeOpen
// resolves them. A file's hash and its include list are kept per path and reused while its size
// and write time are unchanged. Empty (the caller then uses raEffectContentKey) when an #include
// does not name a file literally.
// Main thread only, as compileEffect is.
namespace {

struct RaKeyFile
{
    bool                     present;
    unsigned int             size;
    unsigned int             writeTimeLow;
    unsigned int             writeTimeHigh;
    unsigned long long       hash;
    std::shared_ptr<const std::string> text;   // deobfuscated, for the effect prefetch
    std::vector<std::string> includes;
    bool                     dynamicInclude;   // an #include whose operand is not a literal name
    unsigned int             checkedFrame;     // [PERF] the frame its file info was last read in
    bool                     checked;
};

std::map<std::string, RaKeyFile> g_raKeyFiles;

// [PERF] The file-info memo. raKeyFile asks the FileManager for the size and write time of every
// file an effect can include, for every effect it keys: 30,051 lookups of the same few dozen
// shader headers at the menu switch (2.3 s) and 24,552 in the level's geometry part (1.9 s), at
// about 75 us each. A file whose info was read in the current frame is taken as unchanged for
// the rest of that frame. A long load step is one frame, so each header is looked up about once
// per step; an edit to a shader between frames is still seen. Only the include files the key
// reads use the memo; the cache files, which compileEffect writes, are looked up every time.

// The names of a source's #include lines, in order.
void raScanIncludes(const char* text, std::size_t size, std::vector<std::string>& out, bool& dynamic)
{
    std::size_t line = 0;
    while (line < size)
    {
        std::size_t lineEnd = line;
        while (lineEnd < size && text[lineEnd] != '\n')
            ++lineEnd;
        const char* p = text + line;
        const char* const end = text + lineEnd;
        while (p < end && (*p == ' ' || *p == '\t'))
            ++p;
        if (p < end && *p == '#')
        {
            ++p;
            while (p < end && (*p == ' ' || *p == '\t'))
                ++p;
            if (end - p >= 7 && ::strncmp(p, "include", 7) == 0)
            {
                p += 7;
                while (p < end && (*p == ' ' || *p == '\t'))
                    ++p;
                const char close = (p < end && *p == '"') ? '"' : (p < end && *p == '<') ? '>' : 0;
                const char* const nameEnd =
                    close ? static_cast<const char*>(::memchr(p + 1, close, end - p - 1)) : 0;
                if (nameEnd)
                    out.push_back(std::string(p + 1, nameEnd));
                else
                    dynamic = true;
            }
        }
        line = lineEnd + 1;
    }
}

const RaKeyFile& raKeyFile(const std::string& path, bool obfuscated)
{
    RaKeyFile& file = g_raKeyFiles[path];
    // [PERF] The file-info memo: already read this frame.
    const bool memo = g_Renderer != 0;
    const unsigned int frame = memo ? rendererFrameOrdinal() : 0;
    if (memo && file.checked && file.checkedFrame == frame && file.hash != 0)
        return file;
    RaFileInfo info;
    const bool present = raGetFileInfo(path, info);
    file.checked = memo;
    file.checkedFrame = frame;
    if (file.present == present && file.hash != 0
        && (!present || (file.size == info.size && file.writeTimeLow == info.writeTimeLow
                         && file.writeTimeHigh == info.writeTimeHigh)))
        return file;

    file.present = false;
    file.size = info.size;
    file.writeTimeLow = info.writeTimeLow;
    file.writeTimeHigh = info.writeTimeHigh;
    file.includes.clear();
    file.text.reset();
    file.dynamicInclude = false;
    file.hash = raFnv1a(14695981039346656037ull, "missing", 7);

    renddx9abi::HostStream* const stream = present ? raOpen(path, 2, 0) : 0;
    if (!stream)
        return file;
    std::vector<unsigned char> text(info.size + 1, 0);
    const unsigned int read = info.size ? stream->read(&text[0], info.size) : 0;
    stream->release();
    if (read != info.size)
        return file;
    if (obfuscated && info.size)
        deobfuscateShader(&text[0], info.size);

    file.present = true;
    file.hash = raFnv1a(14695981039346656037ull, &text[0], info.size);
    file.text = std::make_shared<const std::string>(reinterpret_cast<const char*>(&text[0]), info.size);
    raScanIncludes(reinterpret_cast<const char*>(&text[0]), info.size, file.includes,
                   file.dynamicInclude);
    return file;
}

// One file the effect can include, by the name its #include line gives.
struct RaIncludeText
{
    std::string                        path;       // as raIncludeOpen resolves it; empty for RaDefines
    std::shared_ptr<const std::string> text;       // null when the file is missing
};
typedef std::map<std::string, RaIncludeText> RaIncludeMap;

} // anonymous namespace

// includesOut, when given, receives every reachable include with its text (the effect prefetch
// compiles from these).
std::string raEffectSourceKey(const char* source, unsigned int sourceLen, const D3DXMACRO* defines,
                              unsigned long flags, const std::string& raDefines, bool obfuscated,
                              RaIncludeMap* includesOut = 0)
{
    unsigned long long hash = raFnv1a(14695981039346656037ull, source, sourceLen);
    bool dynamic = false;
    std::vector<std::string> pending;
    raScanIncludes(source, sourceLen, pending, dynamic);

    std::set<std::string> seen;
    for (std::size_t i = 0; i < pending.size() && !dynamic; ++i)
    {
        const std::string name = pending[i];   // a copy: pending grows below
        hash = raFnv1a(hash, name.c_str(), name.size() + 1);
        if (::_stricmp(name.c_str(), "shaders/RaDefines.fx") == 0
            || ::_stricmp(name.c_str(), "mods/xpack/shaders/RaDefines.fx") == 0)
        {
            hash = raFnv1a(hash, raDefines.data(), raDefines.size());
            if (includesOut)
                (*includesOut)[name].text = std::make_shared<const std::string>(raDefines);
            continue;
        }
        std::string path(name);
        if (g_Renderer)
            applyModPrefix(path);
        const bool first = seen.insert(path).second;
        const RaKeyFile& file = first ? raKeyFile(path, obfuscated) : g_raKeyFiles[path];
        if (includesOut)
        {
            RaIncludeText& entry = (*includesOut)[name];
            entry.path = path;
            entry.text = file.present ? file.text : std::shared_ptr<const std::string>();
        }
        if (!first)
            continue;
        hash = raFnv1a(hash, &file.hash, sizeof(file.hash));
        dynamic = file.dynamicInclude;
        pending.insert(pending.end(), file.includes.begin(), file.includes.end());
    }
    if (dynamic)
        return std::string();

    for (const D3DXMACRO* define = defines; define && define->Name; ++define)
    {
        hash = raFnv1a(hash, define->Name, ::strlen(define->Name) + 1);
        if (define->Definition)
            hash = raFnv1a(hash, define->Definition, ::strlen(define->Definition) + 1);
    }
    const unsigned int flagWord = static_cast<unsigned int>(flags);
    hash = raFnv1a(hash, &flagWord, sizeof(flagWord));
    const unsigned char obfuscatedByte = obfuscated ? 1 : 0;
    hash = raFnv1a(hash, &obfuscatedByte, 1);

    char hex[18];
    ::sprintf_s(hex, "s%016llx", hash);   // "s": never equal to a raEffectContentKey key
    return std::string(hex);
}

// [PERF] The Ra effect prefetch. The serial path compiles each effect when getEffect first asks
// for it, one after the other on the main thread: 1338 effects, about 3 minutes, on a first load of
// muttrah_city_2. The names asked for are appended to effects.lst in the cache directory. At the
// first compile of a session, the main thread reads each listed effect, its includes and its cache
// key, and every one whose cache is stale goes to worker threads, which compile it with the first
// profile the serial loop would try (D3DX compiles without the device). When getEffect reaches the
// name, compileEffect takes that bytecode in place of compiling it and carries on as before:
// creating the effect, the technique test, the cache write. A job is used only when its key equals
// the key compileEffect computes, so its source, includes, defines and flags are the ones the
// serial compile would see. The include watches the serial include handler registers are
// registered from the list of files the worker opened. A job whose compile failed, or that asked
// for a file it was not given, is dropped and the serial compile runs. Windows only (the compiler entry is resolved from d3dx9_24.dll).
namespace {

// PE 0x100376C8: insert before registering, and ignore the returned resolved path.
void raWatchInclude(EffectSubObject* sub, const std::string& path)
{
    if (sub->watched.find(path) != sub->watched.end())
        return;
    sub->watched.insert(path);
#if !defined(_MSC_VER)
    // [CHOSEN, not recovered] The native provider uses its C++ interface.
    static_cast<io::IFileChangeManager*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->addListener(reinterpret_cast<io::IFileChangeListener*>(&sub->vptr2), path, 3);
#else
    static_cast<IFileChangeManagerPe*>(static_cast<void*>(::dice::hfe::fileChangeManager))
        ->addListener(reinterpret_cast<IBase*>(&sub->vptr2), path, 3);
#endif
}

#if defined(_WIN32) && !defined(__APPLE__)
struct RaPrefetchJob
{
    // Inputs, written by the main thread before the job is queued.
    std::string   name;
    std::string   key;
    std::string   source;
    std::string   defineText[2 * kRaProfileDefines];
    D3DXMACRO     defines[kRaProfileDefines + 1];
    unsigned long flags;
    RaIncludeMap  includes;
    // Results, written by one worker and read by the main thread once done is set.
    bool                     done;
    bool                     complete;   // every include it asked for was given
    ID3DXBuffer*             blob;
    std::vector<std::string> opened;     // include paths, in the order the compiler opened them

    RaPrefetchJob() : flags(0), done(false), complete(true), blob(0) {}
};

// Serves a job's includes from memory, as raIncludeOpen would have read them.
class RaMemoryInclude : public ID3DXInclude
{
public:
    explicit RaMemoryInclude(RaPrefetchJob& job) : m_job(job) {}
    STDMETHOD(Open)(D3DXINCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes)
    {
        const RaIncludeMap::const_iterator it = name ? m_job.includes.find(name) : m_job.includes.end();
        if (it == m_job.includes.end() || !it->second.text)
        {
            m_job.complete = false;
            return E_FAIL;
        }
        if (!it->second.path.empty())
            m_job.opened.push_back(it->second.path);
        *data  = it->second.text->c_str();
        *bytes = static_cast<UINT>(it->second.text->size());
        return S_OK;
    }
    STDMETHOD(Close)(LPCVOID) { return S_OK; }

private:
    RaPrefetchJob& m_job;
};

struct RaPrefetchPool
{
    std::mutex                 mutex;
    std::condition_variable    work;
    std::condition_variable    finished;
    std::deque<RaPrefetchJob*> queue;
};

// Never destroyed: the workers block on it until the process ends, and joining them from a static
// destructor would run under the loader lock.
RaPrefetchPool* g_raPrefetchPool = 0;
std::map<std::string, RaPrefetchJob*> g_raPrefetchJobs;   // main thread only
bool g_raPrefetchStarted = false;
std::set<std::string> g_raManifestNames;                  // main thread only
bool g_raManifestRead = false;

// D3DX's effect compiler runs one compile at a time per loaded module (8 threads on one module:
// no faster than serial; on 8 copies: 4.6 times faster). Each worker therefore loads its own copy
// of the module the main thread compiles with: the same file under another name in a temp
// directory. Null when the copy cannot be made or loaded; the worker then uses the shared module.
RaD3dxCreateEffectCompilerFn raLoadPrivateCompiler(unsigned int index)
{
    char source[MAX_PATH];
    if (!s_raD3dxModule || !::GetModuleFileNameA(s_raD3dxModule, source, MAX_PATH))
        return 0;
    char directory[MAX_PATH];
    const DWORD length = ::GetTempPathA(MAX_PATH, directory);
    if (length == 0 || length > MAX_PATH - 40)
        return 0;
    ::strcat_s(directory, "openbf2-d3dx");
    ::CreateDirectoryA(directory, 0);
    char target[MAX_PATH];
    ::sprintf_s(target, "%s\\d3dx9_24_w%u.dll", directory, index);
    if (!::CopyFileA(source, target, FALSE))
    {
        // Another running game may hold the copy open; it is the same file if the size matches.
        WIN32_FILE_ATTRIBUTE_DATA a, b;
        if (!::GetFileAttributesExA(source, GetFileExInfoStandard, &a)
            || !::GetFileAttributesExA(target, GetFileExInfoStandard, &b)
            || a.nFileSizeLow != b.nFileSizeLow || a.nFileSizeHigh != b.nFileSizeHigh)
            return 0;
    }
    const HMODULE module = ::LoadLibraryA(target);
    return module ? reinterpret_cast<RaD3dxCreateEffectCompilerFn>(
                        ::GetProcAddress(module, "D3DXCreateEffectCompiler"))
                  : 0;
}

void raPrefetchCompile(RaPrefetchJob& job, RaD3dxCreateEffectCompilerFn create)
{
    ::dice::hfe::loadtrace::Scope trace("fx.worker", job.name.c_str());   // [DEV] BF2_LOAD_TRACE
    RaMemoryInclude include(job);
    ID3DXEffectCompiler* compiler = 0;
    ID3DXBuffer* errors = 0;
    const long created = create
        ? create(job.source.c_str(), static_cast<UINT>(job.source.size()), job.defines, &include,
                 job.flags, &compiler, &errors)
        : raCreateEffectCompiler(job.source.c_str(), static_cast<unsigned int>(job.source.size()),
                                 job.defines, &include, job.flags, &compiler, &errors);
    if (created == 0 && compiler)
    {
        if (errors)
        {
            errors->Release();
            errors = 0;
        }
        ID3DXBuffer* blob = 0;
        if (compiler->CompileEffect(job.flags, &blob, &errors) == 0)
            job.blob = blob;
        else if (blob)
            blob->Release();
    }
    if (compiler)
        compiler->Release();
    if (errors)
        errors->Release();
}

void raPrefetchWorker(RaPrefetchPool* pool, RaD3dxCreateEffectCompilerFn create)
{
    for (;;)
    {
        RaPrefetchJob* job = 0;
        {
            std::unique_lock<std::mutex> lock(pool->mutex);
            pool->work.wait(lock, [pool] { return !pool->queue.empty(); });
            job = pool->queue.front();
            pool->queue.pop_front();
        }
        raPrefetchCompile(*job, create);
        {
            std::lock_guard<std::mutex> lock(pool->mutex);
            job->done = true;
        }
        pool->finished.notify_all();
    }
}

void raPrefetchQueue(RaPrefetchJob* job)
{
    if (!g_raPrefetchPool)
    {
        g_raPrefetchPool = new RaPrefetchPool;
        unsigned int threads = std::thread::hardware_concurrency();
        threads = threads > 1 ? threads - 1 : 1;
        if (threads > 8)
            threads = 8;
        unsigned int privateCompilers = 0;
        for (unsigned int i = 0; i < threads; ++i)
        {
            const RaD3dxCreateEffectCompilerFn create = raLoadPrivateCompiler(i);
            privateCompilers += create ? 1 : 0;
            std::thread(raPrefetchWorker, g_raPrefetchPool, create).detach();
        }
        ::fprintf(stderr, "[EFFECTPREFETCH] %u worker threads, %u with their own compiler module\n",
                  threads, privateCompilers);
        ::fflush(stderr);
    }
    g_raPrefetchJobs[job->name] = job;
    {
        std::lock_guard<std::mutex> lock(g_raPrefetchPool->mutex);
        g_raPrefetchPool->queue.push_back(job);
    }
    g_raPrefetchPool->work.notify_one();
}

void raParseManifest(const std::string& text, std::vector<std::string>& names);

// The listed names, read once per session.
void raReadManifest(const std::string& path, std::vector<std::string>& names)
{
    g_raManifestRead = true;
    RaFileInfo info;
    if (!raGetFileInfo(path, info) || info.size == 0)
        return;
    renddx9abi::HostStream* const file = raOpen(path, 2, 0);
    if (!file)
        return;
    std::string text(info.size, '\0');
    const unsigned int read = file->read(&text[0], info.size);
    file->release();
    text.resize(read);
    raParseManifest(text, names);
}

// Appends the names in a manifest's text that are not listed yet.
void raParseManifest(const std::string& text, std::vector<std::string>& names)
{
    std::string::size_type line = 0;
    while (line < text.size())
    {
        std::string::size_type end = text.find('\n', line);
        if (end == std::string::npos)
            end = text.size();
        std::string name(text, line, end - line);
        while (!name.empty() && (name[name.size() - 1] == '\r' || name[name.size() - 1] == ' '))
            name.erase(name.size() - 1);
        if (!name.empty() && g_raManifestNames.insert(name).second)
            names.push_back(name);
        line = end + 1;
    }
}

// A cache directory without a manifest (new, because an engine define changed its name) takes the
// newest manifest among its siblings under cache/, and writes it as its own. Names another directory
// listed are only candidates: a name that does not read is skipped, and a job whose key differs from
// the one compileEffect computes is dropped, so a stale name costs at most one wasted compile.
void raSeedManifest(const std::string& cacheDirectory, const std::string& manifestPath,
                    std::vector<std::string>& names)
{
    if (!::dice::hfe::fileManager)
        return;
    std::string own;
    ::dice::hfe::fileManager->getWritePath(renddx9abi::makeHostString(cacheDirectory), own);
    while (!own.empty() && (own[own.size() - 1] == '/' || own[own.size() - 1] == '\\'))
        own.erase(own.size() - 1);
    const std::string::size_type slash = own.find_last_of("/\\");
    if (slash == std::string::npos)
        return;
    const std::string parent = own.substr(0, slash + 1);
    const std::string ownName = own.substr(slash + 1);

    const auto widen = [](const std::string& text) {
        std::wstring wide;
        const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, 0, 0);
        if (length > 1)
        {
            wide.resize(static_cast<std::size_t>(length - 1));
            ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &wide[0], length);
        }
        return wide;
    };
    const std::wstring parentWide = widen(parent);
    const std::wstring ownWide = widen(ownName);
    WIN32_FIND_DATAW found;
    const HANDLE search = ::FindFirstFileW((parentWide + L"*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    std::wstring newest;
    ULARGE_INTEGER newestTime = {};
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.'
            || ::_wcsicmp(found.cFileName, ownWide.c_str()) == 0)
            continue;
        const std::wstring candidate = parentWide + found.cFileName + L"\\effects.lst";
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!::GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &attributes)
            || attributes.nFileSizeLow == 0)
            continue;
        ULARGE_INTEGER time;
        time.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
        time.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
        if (newest.empty() || time.QuadPart > newestTime.QuadPart)
        {
            newest = candidate;
            newestTime = time;
        }
    } while (::FindNextFileW(search, &found));
    ::FindClose(search);
    if (newest.empty())
        return;

    const HANDLE file = ::CreateFileW(newest.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (file == INVALID_HANDLE_VALUE)
        return;
    std::string text;
    const DWORD size = ::GetFileSize(file, 0);
    if (size != INVALID_FILE_SIZE && size > 0)
    {
        text.resize(size);
        DWORD read = 0;
        if (!::ReadFile(file, &text[0], size, &read, 0))
            read = 0;
        text.resize(read);
    }
    ::CloseHandle(file);
    const std::size_t before = names.size();
    raParseManifest(text, names);
    if (names.size() == before)
        return;
    std::string seeded;
    for (std::size_t i = before; i < names.size(); ++i)
        seeded += names[i] + "\n";
    ::dice::hfe::fileManager->writeFile(renddx9abi::makeHostString(manifestPath), seeded.data(),
                                        static_cast<unsigned int>(seeded.size()), true);
    ::fprintf(stderr, "[EFFECTPREFETCH] seeded %u names from a sibling cache directory\n",
                 static_cast<unsigned int>(names.size() - before));
}

// Appends a name the manifest does not list yet. FileManager::writeFile cannot append (its bool
// picks CREATE_NEW or CREATE_ALWAYS), so the file is opened without truncating (OPEN_ALWAYS) and
// written at its end. File streams are seekable (INonlinearStream, as File.cpp uses them).
void raNoteManifest(const std::string& path, const std::string& name)
{
    if (!::dice::hfe::fileManager || !g_raManifestNames.insert(name).second)
        return;
    const std::string line = name + "\n";
    renddx9abi::HostStream* const file = raOpen(path, 3, 2);   // OPEN_ALWAYS, write
    if (!file)
        return;
    static_cast< ::dice::hfe::io::INonlinearStream*>(file)->seek(::dice::hfe::io::SEEKORIGIN_END, 0);
    file->write(line.data(), static_cast<unsigned int>(line.size()));
    file->release();
}

// The prefetched bytecode for name, or null (compile it serially). Waits for the job if a worker
// still has it. On success the include watches the compile would have registered are registered.
ID3DXBuffer* raTakePrefetched(const std::string& name, const std::string& key, EffectSubObject* sub)
{
    const std::map<std::string, RaPrefetchJob*>::iterator it = g_raPrefetchJobs.find(name);
    if (it == g_raPrefetchJobs.end())
        return 0;
    RaPrefetchJob* const job = it->second;
    g_raPrefetchJobs.erase(it);
    {
        ::dice::hfe::loadtrace::Scope trace("fx.wait", job->name.c_str(), 100);   // [DEV]
        std::unique_lock<std::mutex> lock(g_raPrefetchPool->mutex);
        g_raPrefetchPool->finished.wait(lock, [job] { return job->done; });
    }
    ID3DXBuffer* blob = 0;
    if (job->key == key && job->complete && job->blob)
    {
        blob = job->blob;
        job->blob = 0;
        if (sub)
            for (std::size_t i = 0; i < job->opened.size(); ++i)
                raWatchInclude(sub, job->opened[i]);
    }
    if (job->blob)
        job->blob->Release();
    delete job;
    return blob;
}
#endif // _WIN32 && !__APPLE__

} // anonymous namespace

// The key a previous compile stored beside its blob; empty when there is none.
std::string raReadCacheKey(const std::string& path)
{
    ::dice::hfe::loadtrace::Scope keyFileTrace("fx.keyfile", path.c_str());   // [DEV] BF2_LOAD_TRACE
    RaFileInfo info;
    if (!raGetFileInfo(path, info) || info.size == 0 || info.size > 64)
        return std::string();
    renddx9abi::HostStream* const file = raOpen(path, 2, 0);
    if (!file)
        return std::string();
    char buffer[65];
    const unsigned int read = file->read(buffer, info.size);
    file->release();
    return std::string(buffer, read);
}

bool raWriteCacheKey(const std::string& path, const std::string& key)
{
    renddx9abi::HostStream* const file = raOpen(path, 0, 2);
    if (!file)
        return false;
    file->write(key.data(), static_cast<unsigned int>(key.size()));
    file->release();
    return true;
}


// StringFromGUID2's output, spelled out. The shipped body calls ole32's StringFromGUID2 into 39
// wide characters and then narrows them a byte at a time. This implementation formats the same
// canonical braced upper-case form directly: 38 characters and a NUL, without an ole32 call.
// Byte-for-byte identical output; [CHOSEN, not recovered] as a mechanism.

void raFormatGuid(char out[40], const GUID& guid)
{
    ::sprintf_s(out, 40, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                static_cast<unsigned long>(guid.Data1),
                static_cast<unsigned int>(guid.Data2), static_cast<unsigned int>(guid.Data3),
                guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
                guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

} // anonymous namespace


// Compiles source with this manager's include resolver and supplied macro definitions.
// Returns a compiled-effect buffer, or null with compiler diagnostics in errors.
// The optional second pass fixes projected-texture shaders for pixel shader 1.4.
// PE 0x100344B0.
ID3DXBuffer* EffectManager::compileEffectSource(const char* source, unsigned int sourceLen,
                                                const D3DXMACRO* defines, unsigned long* flags,
                                                ID3DXBuffer** errors)
{
    {
        // The shipped expression, with the four copy-constructed temporaries MSVC emitted for it
        // collapsed: `text = std::string("#line 2 \"Generated.fx\"\r\n") + source`.
        std::string generated("#line 2 \"Generated.fx\"\r\n");
        generated += source;
        m_sub->text = generated;
    }

    ID3DXBuffer* blob = 0;

    for (int pass = 1; ; ++pass)
    {
        ID3DXEffectCompiler* compiler = 0;

        // sub_1003C610 - the lazily-bound D3DXCreateEffectCompiler in d3dx9_24.dll, with `pInclude`
        // set to this object's +0x44 sub-object, whose +0x00 is the off_101E139C include table.
        const long created = raCreateEffectCompiler(
            source, sourceLen, defines, reinterpret_cast<LPD3DXINCLUDE>(m_sub), *flags, &compiler,
            errors);
        if (created != 0)
        {
            raRelease(compiler);
            return 0;
        }

        raRelease(*errors);

        blob = 0;
        const long hr = compiler->CompileEffect(*flags, &blob, errors);
        raRelease(compiler);

        if (hr != 0)
        {
            if (blob)
                blob->Release();
            return 0;
        }

        if (pass > 1)
            break;
        if (!blob)
            return 0;


        // PE 0x100345C4..0x100345FF: preserve the flag and the shipped macro-index test.
        // The compatibility test reads entry 3, rather than PSVERSION at entry 5.

        if (!m_byte61
            || ::strcmp(defines[3].Name, "PSVERSION") != 0
            || ::strcmp(defines[3].Definition, "14") != 0)
        {
            break;
        }

        // PE 0x100345FF..0x10034896. These are function-local static PME patterns in retail.
        static RaProjectedPattern projectedFlags(
            "^\\s*TextureTransformFlags\\s*\\[([0-6])\\]\\s*=.*PROJECTED.*;.*");
        projectedFlags.reset();
        std::vector<std::string> projectedRegisters;
        while (projectedFlags.match(std::string(m_sub->text.c_str())))
            projectedRegisters.push_back(std::string("r") + projectedFlags.group(1));
        if (projectedRegisters.empty())
            return blob;

        static RaProjectedPattern projectedCall("=\\s*tex2Dproj\\s*\\(.*\\)");
        if (!projectedCall.match(std::string(m_sub->text.c_str())))
            return blob;

        static RaProjectedPattern compiledPixelShader(
            "^\\s*PixelShader\\s*=\\s*compile\\s+(ps_1_4|M?PSMODEL)\\s+.+\\(.*\\)\\s*;");
        compiledPixelShader.reset();
        if (!compiledPixelShader.match(std::string(m_sub->text.c_str())))
            return blob;
        const std::pair<size_t, size_t> insertion = compiledPixelShader.span(0);
        m_sub->text.erase(insertion.first, insertion.second - insertion.first);

#if defined(_WIN32)
        // PE 0x1003489C..0x10034946. Shipped defect: the disassembly failure describes the
        // GetPassDesc result, rather than the failing D3DXDisassembleEffect HRESULT.
        ID3DXEffect* effect = 0;
        const HRESULT createdEffect = D3DXCreateEffect(m_device, blob->GetBufferPointer(),
            blob->GetBufferSize(), 0, 0, static_cast<DWORD>(*flags & ~0x800u),
            static_cast<ID3DXEffectPool*>(m_effectPool), &effect, 0);
        if (createdEffect != 0)
        {
            blob->Release();
            raRelease(effect);
            return 0;
        }
        const D3DXHANDLE technique = effect->GetTechnique(0);
        const D3DXHANDLE effectPass = effect->GetPass(technique, 0);
        D3DXPASS_DESC passDescription;
        const HRESULT describedPass = effect->GetPassDesc(effectPass, &passDescription);
        const DWORD* const pixelShader = passDescription.pPixelShaderFunction;
        ID3DXBuffer* disassembly = 0;
        if (D3DXDisassembleEffect(effect, FALSE, &disassembly) != 0)
        {
            ::renddx9abi::logf("Ra/EffectManager.cpp:672 Failed to disasm. %s",
                               DXGetErrorDescription9A(describedPass));
            raRelease(effect);
            raRelease(blob);
            raRelease(disassembly);
            return 0;
        }
        const std::string disassembled(static_cast<const char*>(disassembly->GetBufferPointer()));
        raRelease(disassembly);

        // PE 0x1003494B..0x10034B43. Only the first texld match per projected register changes.
        static RaProjectedPattern assembledPixelShader(
            "^\\s*PixelShader\\s*=\\s*asm\\s*{.*}\\s*;", true);
        assembledPixelShader.reset();
        std::string assembly;
        if (assembledPixelShader.match(std::string(disassembled.c_str())))
        {
            assembly = assembledPixelShader.group(0);
            bool changed = false;
            for (size_t i = 0; i < projectedRegisters.size(); ++i)
            {
                RaProjectedPattern load(std::string("^\\s*texld\\s+") + projectedRegisters[i]
                    + ",\\s*t[0-6]_dw(\\.xyww)", true);
                if (load.match(std::string(assembly.c_str())))
                {
                    const std::pair<size_t, size_t> swizzle = load.span(1);
                    assembly.replace(swizzle.first, swizzle.second - swizzle.first, ".xyzw");
                    changed = true;
                }
            }
            if (!changed)
            {
                raRelease(effect);
                return blob;
            }
        }
        // Shipped defect: absence of an assembly match still takes the recompile path,
        // inserting an empty assembly string after removing the compiled shader statement.
        m_sub->text.insert(insertion.first, assembly);

        // PE 0x10034B4E..0x10034D51. The table and its description result are unchecked.
        ID3DXConstantTable* constants = 0;
        D3DXGetShaderConstantTable(pixelShader, &constants);
        std::string bindings;
        unsigned int index = 0;
        for (D3DXHANDLE constant = constants->GetConstant(0, index); constant;
             constant = constants->GetConstant(0, ++index))
        {
            D3DXCONSTANT_DESC descriptions[16];
            ::memset(descriptions, 0, sizeof(descriptions));
            UINT count = 16;
            constants->GetConstantDesc(constant, descriptions, &count);
            const char* format = 0;
            switch (descriptions[0].RegisterSet)
            {
            case D3DXRS_SAMPLER: format = "Sampler[%i] = (%s);\r\n"; break;
            case D3DXRS_FLOAT4: format = "PixelShaderConstantF[%i] = (%s);\r\n"; break;
            case D3DXRS_INT4: format = "PixelShaderConstantI[%i] = (%s);\r\n"; break;
            case D3DXRS_BOOL: format = "PixelShaderConstantB[%i] = (%s);\r\n"; break;
            default:
                ::renddx9abi::logf("Ra/EffectManager.cpp:770 Unknown register set.");
                break;
            }
            if (format)
                bindings += ::dice::hfe::formatString(format,
                    descriptions[0].RegisterIndex, descriptions[0].Name);
        }
        m_sub->text.insert(insertion.first, bindings);
        raRelease(constants);
        raRelease(effect);
        m_sub->text = m_sub->text + "";
        source = m_sub->text.c_str();
        sourceLen = static_cast<unsigned int>(m_sub->text.size());
        raRelease(blob);
#else
        // [CHOSEN, not recovered] The native D3DX backend has no effect disassembler.
        // Refuse this compilation rather than returning a blob without the required rewrite.
        ::fprintf(stderr, "[CHOSEN, not recovered] dice::hfe::dx9rend::Ra::EffectManager::compileEffectSource: "
                          "projected ps_1_4 rewrite unavailable; compilation refused\n");
        ::fflush(stderr);
        blob->Release();
        return 0;
#endif
    }

    // PE 0x10034DE7: clear the expanded copy and hand the blob back.
    m_sub->text.clear();
    return blob;
}


// Loads or compiles an effect and replaces the reference in effectOut.
// Cache identity includes the adapter, build number and filter quality; stale caches
// are recompiled across supported shader models. unused is retained for ABI compatibility.

void* EffectManager::compileEffect(const std::string& name, int unused, void** effectOut)
{
    // PE 0x10034F60.
    (void)unused;   // arg_4: passed by getEffect as 0, never read by the shipped body
    // [DEV] BF2_LOAD_TRACE: the whole effect; fx.key, fx.cacheload, fx.compile and fx.create below.
    ::dice::hfe::loadtrace::Scope loadTrace("fx.ra", name.c_str());

    loadStat->m_unknown08 = true;

    // [DEV] BF2_LOAD_TRACE: fx.setup is everything before the source read (flags, adapter GUID,
    // video options, mod directory, cache path).
    const long long setupT0 = ::dice::hfe::loadtrace::enabled() ? ::dice::hfe::loadtrace::now() : -1;
    unsigned long flags = 0;
    if (g_noPreshader)      flags  = 0x100u;
    if (g_shaderDebug)      flags |= 0x001u;
    if (g_skipOptimization) flags |= 0x004u;
    if (g_partialPrecision) flags |= 0x020u;

    // sub_10001260 on dword_1023B980 - sixteen bytes at DeviceCaps + 1384, which is
    // D3DADAPTER_IDENTIFIER9::DeviceIdentifier (the adapter block starts at +304 and the GUID is
    // 1080 bytes into it; the same arithmetic puts VendorId at +1368, which is the dword the
    // isNvidia/isAti pair above already reads). The shipped body then StringFromGUID2s it into 39
    // wide characters and copies them one byte at a time - a deliberate narrowing, not a
    // conversion. See raFormatGuid for why the OLE call itself is not made here.
    char guidText[40];
    guidText[0] = '\0';
    {
        const CapsView* const caps = capsOf();
        if (caps)
            raFormatGuid(guidText, deviceCapsDeviceIdentifier());          // sub_10001260
    }

    std::string modPath("mods/bf2");
    if (g_Renderer && static_cast<IRenderer*>(g_Renderer)->isExpansionPack())
        modPath = "mods/xpack";
    // [FIX] Retail keys the cache by bf2/xpack only, so every mod shares
    // mods/bf2/cache. The freshness test compares against the mod's .fx timestamps, and mod archives
    // usually carry older timestamps than a cache from an earlier vanilla run. Mods with their own
    // shaders then load stale vanilla blobs. Project Reality's log-depth shaders lose the depth test
    // against vanilla-compiled meshes. Any non-vanilla mod gets its own cache directory.
    {
        std::string modDirectory;
        readStringSettingDefault("GSModDirectory", "", modDirectory);
        while (!modDirectory.empty()
               && (modDirectory[modDirectory.size() - 1] == '/' || modDirectory[modDirectory.size() - 1] == '\\'))
            modDirectory.erase(modDirectory.size() - 1);
        std::string lowered(modDirectory);
        for (std::size_t i = 0; i < lowered.size(); ++i)
        {
            const char c = lowered[i];
            lowered[i] = (c == '\\') ? '/' : ((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        if (!lowered.empty() && lowered != "mods/bf2" && lowered != "mods/xpack")
            modPath = lowered;
    }

    // Shipped: the cache is under @HOME@ (PE 0x10035054 pushes "@HOME@/"), not the game folder.
    // Retail BF2.exe expands @HOME@ to SHGetFolderPathW(CSIDL_PERSONAL) + "\Battlefield 2" (PE
    // 0x6C5A80), so Windows writes Documents\Battlefield 2\mods\bf2\cache\. The macOS client data
    // directory stands in for that folder (docs/archive/macos-write-paths.md).
    std::string cachePath("@HOME@/");
    cachePath += modPath;
    cachePath += "/cache/";
    {
        char suffix[64];
        // sub_10074AB0, "_%d_%d", 3153, q: retail keys the directory by VideoOptions quality 7.
        // [RENDER] No define depends on it any more (samplers state their own filter), so q is
        // fixed at 0 and one cache serves every setting.
        ::sprintf_s(suffix, "_%d_%d", 0xC51, 0);
        cachePath += guidText;
        cachePath += suffix;
    }
    cachePath += "/";
    const std::string cacheDirectory(cachePath);   // [PERF] the effect prefetch's manifest lives here
    {
        std::string stem;
        raCacheStem(stem, name);
        cachePath += stem;
    }
    cachePath += ".cfx";

    // The shader profiles the compile tries, in order (see the compile loop below).
    const unsigned int psVersion = (deviceCapsPixelShaderMajor() << 8) | psMinor();
    // GSQuickShaderPermutation starts the pixel-model search at ps_2_a instead of ps_1_3.
    const bool quick = readBoolSetting("GSQuickShaderPermutation", false);
    // THE TWO skips ARE PER-vendor AND they ARE NOT symmetric.
    //   ps_2_a (index 2) is skipped on an ATI part that lacks PS20Caps.Caps bit 0
    //                    (D3DPS20CAPS_ARBITRARYSWIZZLE, sub_10001450 = caps[66] & 1)
    //   ps_2_b (index 3) is skipped on any nvidia part
    const bool skipPs2a = isAti() && !ps20ArbitrarySwizzle();
    const bool skipPs2b = isNvidia();
    // _FORCE_1_4_SHADERS_. It is NOT the same expression the permutator's own macro builder
    // uses (sub_1003CA30 omits the debug byte); both are shipped, in different bodies.
    const unsigned int forceIndex =
        (psVersion < 0x200u || g_forceOneFour || m_flag40) ? 1u : 0u;
    const int firstPs = quick ? 3 : 0;
    const auto psProfileUsable = [&](int psIndex) {
        return !(skipPs2a && psIndex == 2) && !(skipPs2b && psIndex == 3)
            && psVersion >= kMinPsVersion[psIndex];
    };
    const auto fillProfileDefines = [&](int vsIndex, int psIndex, D3DXMACRO* defines) {
        defines[0].Name = kVsModel[vsIndex][0];  defines[0].Definition = kVsModel[vsIndex][1];
        defines[1].Name = kPsModel[psIndex][0];  defines[1].Definition = kPsModel[psIndex][1];
        defines[2].Name = kForce14[forceIndex][0];
        defines[2].Definition = kForce14[forceIndex][1];
        defines[3].Name = "_FORCE_1_3_SHADERS_"; defines[3].Definition = "0";
        defines[4].Name = "LOWPSMODEL";          defines[4].Definition = "ps_1_4";
        defines[5].Name = kPsVersion[psIndex][0];
        defines[5].Definition = kPsVersion[psIndex][1];
        // [RENDER] The renderer contract version (RendDX9Base.h), not retail.
        defines[6].Name = "RA_CONTRACT";
        defines[6].Definition = RA_CONTRACT_TEXT(RA_CONTRACT_VERSION);
        defines[7].Name = 0;                     defines[7].Definition = 0;
    };

    if (setupT0 >= 0)   // [DEV] BF2_LOAD_TRACE
        ::dice::hfe::loadtrace::span("fx.setup", name.c_str(), setupT0, ::dice::hfe::loadtrace::now());
    unsigned char* source = 0;
    const unsigned int sourceLen = readEffectFile(name, &source);
    if (sourceLen == 0)
    {
        // shipped defect: this early return leaves suppression set and skips the progress event.
        *effectOut = 0;
        ::renddx9abi::engineFree(source);
        return 0;
    }
    // GSLoadObfuscated, read per call in the shipped body with its default byte seeded to 1.
    const bool obfuscated = readBoolSetting("GSLoadObfuscated", true);
    if (obfuscated)
        deobfuscateShader(source, sourceLen);

    // [FIX] Content-keyed cache. Retail reuses the .cfx while it is newer than
    // the .fx: an edited include never invalidates it, and a generated permutation (no file on
    // disk) is never recompiled. Here the key is a hash of the effect as the compiler sees it - the
    // source expanded through the same include handler with the first profile's defines - plus
    // the compile flags. It lives in <cache>.key, and the blob is used only when the key matches.
    // this+0x78 is the _Mysize of the string at +0x64 - m_debugDefines - so the cache is used only
    // when NO debug define is active, which is what makes the cached blob safe to reuse at all.
    std::string cacheKey;
    int keyPs = 5;   // the first profile the compile loop tries; 5 when the cache is not used
    if (!m_byte60 && m_debugDefines.size() == 0)
    {
        keyPs = firstPs;
        while (keyPs < 5 && !psProfileUsable(keyPs))
            ++keyPs;
        if (keyPs < 5)
        {
            ::dice::hfe::loadtrace::Scope keyTrace("fx.key", name.c_str());   // [DEV] BF2_LOAD_TRACE
            D3DXMACRO keyDefines[kRaProfileDefines + 1];
            fillProfileDefines(0, keyPs, keyDefines);
            // [PERF] The source key; the preprocessor key only where it cannot be formed.
            cacheKey = raEffectSourceKey(reinterpret_cast<const char*>(source), sourceLen,
                                         keyDefines, flags, m_shaderDefines + m_debugDefines,
                                         obfuscated);
            if (cacheKey.empty())
                cacheKey = raEffectContentKey(reinterpret_cast<const char*>(source), sourceLen,
                                              keyDefines, reinterpret_cast<LPD3DXINCLUDE>(m_sub),
                                              flags);
        }
    }
    const std::string cacheKeyPath = cachePath + ".key";

#if defined(_WIN32) && !defined(__APPLE__)
    // [PERF] The effect prefetch (see raTakePrefetched): list this name, and at the first compile of
    // the session queue every listed effect whose cache is stale.
    if (keyPs < 5 && !cacheKey.empty())
    {
        const std::string manifestPath = cacheDirectory + "effects.lst";
        std::vector<std::string> listed;
        if (!g_raManifestRead)
        {
            raReadManifest(manifestPath, listed);
            if (listed.empty())
                raSeedManifest(cacheDirectory, manifestPath, listed);
        }
        raNoteManifest(manifestPath, name);
        if (!g_raPrefetchStarted && raLoadEffectCompiler())
        {
            g_raPrefetchStarted = true;
            ::dice::hfe::loadtrace::Scope prepTrace("fx.prefetch", "queue");   // [DEV] BF2_LOAD_TRACE
            D3DXMACRO keyDefines[kRaProfileDefines + 1];
            fillProfileDefines(0, keyPs, keyDefines);
            const std::string raDefines = m_shaderDefines + m_debugDefines;
            unsigned int queued = 0, current = 0, unreadable = 0;
            // A generated permutation reads only once its metashader is loaded, which the mesh
            // code does when it first initializes, after this. So when a listed name does not
            // read, the metashaders whose names start it are loaded now (the load would load them
            // later anyway) and the name is tried once more.
            std::vector<std::string> metaShaders;
            bool metaShadersListed = false;
            std::set<std::string> metaShadersLoaded;
            for (std::size_t i = 0; i < listed.size(); ++i)
            {
                const std::string& other = listed[i];
                if (other == name)
                    continue;
                unsigned char* otherSource = 0;
                unsigned int otherLen = readEffectFile(other, &otherSource);
                if (otherLen == 0)
                {
                    ::renddx9abi::engineFree(otherSource);
                    otherSource = 0;
                    if (!metaShadersListed && ::dice::hfe::fileManager)
                    {
                        metaShadersListed = true;
                        std::list< ::dice::hfe::io::FileInfo> files;
                        ::dice::hfe::fileManager->findFiles(
                            ::renddx9abi::makeHostString(std::string("shaders/")), false, files);
                        for (std::list< ::dice::hfe::io::FileInfo>::const_iterator f = files.begin();
                             f != files.end(); ++f)
                            if (equalsIgnoreCase(extractFileExt(f->name), std::string("mfx"), 3))
                                metaShaders.push_back(std::string("shaders/") + f->name.c_str());
                    }
                    std::string lowered(other);
                    pathToLower(lowered);
                    bool loadedOne = false;
                    for (std::size_t m = 0; m < metaShaders.size(); ++m)
                    {
                        std::string stem;
                        pathStripExtension(stem, metaShaders[m]);
                        pathToLower(stem);
                        if (lowered.compare(0, stem.size(), stem) == 0
                            && metaShadersLoaded.insert(stem).second)
                        {
                            getMetaFx(metaShaders[m]);
                            loadedOne = true;
                        }
                    }
                    if (loadedOne)
                        otherLen = readEffectFile(other, &otherSource);
                    if (otherLen == 0)
                    {
                        ::renddx9abi::engineFree(otherSource);
                        ++unreadable;
                        continue;
                    }
                }
                if (obfuscated)
                    deobfuscateShader(otherSource, otherLen);

                RaPrefetchJob* const job = new RaPrefetchJob;
                job->name = other;
                job->source.assign(reinterpret_cast<const char*>(otherSource), otherLen);
                ::renddx9abi::engineFree(otherSource);
                job->key = raEffectSourceKey(job->source.c_str(), otherLen, keyDefines, flags,
                                             raDefines, obfuscated, &job->includes);
                std::string otherStem;
                raCacheStem(otherStem, other);
                const std::string otherCache = cacheDirectory + otherStem + ".cfx";
                RaFileInfo otherInfo;
                if (job->key.empty()
                    || (raGetFileInfo(otherCache, otherInfo)
                        && raReadCacheKey(otherCache + ".key") == job->key))
                {
                    delete job;
                    ++current;
                    continue;
                }
                int d = 0;
                for (; d < kRaProfileDefines && keyDefines[d].Name; ++d)
                {
                    job->defineText[2 * d] = keyDefines[d].Name;
                    job->defineText[2 * d + 1] = keyDefines[d].Definition ? keyDefines[d].Definition : "";
                    job->defines[d].Name = job->defineText[2 * d].c_str();
                    job->defines[d].Definition =
                        keyDefines[d].Definition ? job->defineText[2 * d + 1].c_str() : 0;
                }
                job->defines[d].Name = 0;
                job->defines[d].Definition = 0;
                job->flags = flags;
                raPrefetchQueue(job);
                ++queued;
            }
            ::fprintf(stderr, "[EFFECTPREFETCH] %u listed: %u queued on worker threads, %u cached, "
                              "%u unreadable now\n", static_cast<unsigned>(listed.size()), queued,
                      current, unreadable);
            ::fflush(stderr);
        }
    }
#endif

    bool useCache = false;
    RaFileInfo cacheInfo;
    cacheInfo.size = 0;
    cacheInfo.writeTimeLow = 0;
    cacheInfo.writeTimeHigh = 0;
    if (!cacheKey.empty() && raGetFileInfo(cachePath, cacheInfo))
        useCache = raReadCacheKey(cacheKeyPath) == cacheKey;

    ID3DXEffect* effect    = 0;
    ID3DXBuffer* errors    = 0;
    D3DXHANDLE   technique = 0;

    if (useCache)
    {
        ::dice::hfe::loadtrace::Scope cacheTrace("fx.cacheload", name.c_str());   // [DEV] BF2_LOAD_TRACE
        // Read the cached effect again into D3DX-owned storage.
        // Local failure guards report a bad cache and leave a null effect for build failure.
        ID3DXBuffer* buffer = 0;
        if (D3DXCreateBuffer(cacheInfo.size, &buffer) != 0)
            ::renddx9abi::logf("EffectManager.cpp:1000 (D3DXCreateBuffer(%u) failed for \"%s\")",
                               cacheInfo.size, cachePath.c_str());

        renddx9abi::HostStream* const file = raOpen(cachePath, 2, 0);
        if (file && buffer)
        {
            void* const dst = buffer->GetBufferPointer();
            const unsigned int read = file->read(dst, cacheInfo.size);
            if (read != cacheInfo.size)
                ::renddx9abi::logf("EffectManager.cpp:1004 (read %u of %u from \"%s\")",
                                   read, cacheInfo.size, cachePath.c_str());
        }
        if (file)
            file->release();

        if (buffer)
        {
            const unsigned int size = buffer->GetBufferSize();
            char* const bytes = static_cast<char*>(buffer->GetBufferPointer());
            for (unsigned int i = 0; i < size; ++i)
                bytes[i] = static_cast<char>(bytes[i] ^ static_cast<char>(i));

            const unsigned long createFlags = flags | (m_byte62 ? 0x800u : 0u);
            const long long createT0 = ::dice::hfe::loadtrace::now();   // [DEV] BF2_LOAD_TRACE
            const long hr = D3DXCreateEffect(
                m_device,
                buffer->GetBufferPointer(),
                buffer->GetBufferSize(),
                0, 0, createFlags, static_cast<LPD3DXEFFECTPOOL>(m_effectPool), &effect, &errors);
            if (::dice::hfe::loadtrace::enabled())
                ::dice::hfe::loadtrace::span("fx.create", name.c_str(), createT0,
                                             ::dice::hfe::loadtrace::now());
            if (hr != 0)
                ::renddx9abi::logf("EffectManager.cpp:1016 Failed to create effect: hr=0x%08X (%s)",
                                   static_cast<unsigned>(hr), cachePath.c_str());
        }

        if (effect)
        {
            technique = effect->GetTechniqueByName("defaultTechnique");
            if (!technique)
                effect->FindNextValidTechnique(0, &technique);
            effect->OnLostDevice();

            raRelease(buffer);

            // AND then IT does IT again. When the +0xBC query says a device reset happened the
            // body runs OnResetDevice, re-finds the technique and calls OnLostDevice a second time.
            // Both OnLostDevice calls are shipped and both are kept.
            if (g_Renderer && rendererDeviceWasReset())
            {
                effect->OnResetDevice();
                technique = effect->GetTechniqueByName("defaultTechnique");
                if (!technique)
                    effect->FindNextValidTechnique(0, &technique);
                effect->OnLostDevice();
            }
        }
        else
        {
            raRelease(buffer);
        }
    }
    else
    {
        for (int vsIndex = 0; vsIndex < 2; )
        {
            int psIndex = firstPs;
            for (; psIndex < 5; ++psIndex)
            {
                if (!psProfileUsable(psIndex)) continue;

                // A device reset restarts both profile indices and immediately compiles index zero.
                if (g_Renderer && rendererDeviceWasReset())
                {
                    vsIndex = 0;
                    psIndex = 0;
                }

                D3DXMACRO defines[kRaProfileDefines + 1];
                fillProfileDefines(vsIndex, psIndex, defines);

                raRelease(errors);

                unsigned long passFlags = flags;      // the shipped body passes a copy by pointer
                const long long compileT0 = ::dice::hfe::loadtrace::enabled() ? ::dice::hfe::loadtrace::now() : -1;   // [DEV]
                ID3DXBuffer* blob = 0;
#if defined(_WIN32) && !defined(__APPLE__)
                // [PERF] The first attempt uses the prefetched bytecode when there is one.
                if (vsIndex == 0 && psIndex == keyPs && !cacheKey.empty())
                    blob = raTakePrefetched(name, cacheKey, m_sub);
#endif
                if (!blob)
                    blob = compileEffectSource(
                        reinterpret_cast<const char*>(source), sourceLen, defines, &passFlags, &errors);
                if (compileT0 >= 0)   // [DEV] BF2_LOAD_TRACE: one span per profile attempted
                    ::dice::hfe::loadtrace::span(blob ? "fx.compile" : "fx.compile.fail", name.c_str(), compileT0,
                                                ::dice::hfe::loadtrace::now());
                if (!blob)
                    continue;

                const unsigned long createFlags = flags | (m_byte62 ? 0x800u : 0u);
                raRelease(errors);

                const long long createT0 = ::dice::hfe::loadtrace::enabled() ? ::dice::hfe::loadtrace::now() : -1;
                const long hr = D3DXCreateEffect(
                    m_device,
                    blob->GetBufferPointer(),
                    blob->GetBufferSize(),
                    0, 0, createFlags, static_cast<LPD3DXEFFECTPOOL>(m_effectPool), &effect,
                    &errors);
                if (createT0 >= 0)   // [DEV] BF2_LOAD_TRACE
                    ::dice::hfe::loadtrace::span("fx.create", name.c_str(), createT0, ::dice::hfe::loadtrace::now());

                // NOT reproduced, AND nothing IS lost. 0x1003573D and 0x10035743 call
                // DXGetErrorStringA (sub_1005F380, 48,672 B) and DXGetErrorDescription9A
                // (9,617 B) on `hr` and discard both results - the DXTRACE_ERR macro with its
                // output compiled out. Both are DXErr9 library code, not dice's.
                (void)hr;

                if (!effect)
                    continue;

                effect->FindNextValidTechnique(0, &technique);
                effect->OnLostDevice();

                if (technique)
                {
                    g_anyEffectCompiled = 1;                  // byte_1023D950
                    if (writeEffectCache(blob, cachePath))
                        raWriteCacheKey(cacheKeyPath, cacheKey.empty() ? std::string("-") : cacheKey);
                    blob->Release();
                    goto compiled;
                }

                blob->Release();
                raRelease(effect);
            }
            ++vsIndex;
        }
    }

compiled:
    if (effect)
    {
        // Release the caller's previous effect before replacing it.
        if (*effectOut)
            static_cast<ID3DXEffect*>(*effectOut)->Release();
        *effectOut = effect;


        ::dice::hfe::loadtrace::Scope listenTrace("fx.listen", name.c_str());   // [DEV] BF2_LOAD_TRACE
#if !defined(_MSC_VER)
        static_cast<io::IFileChangeManager*>(static_cast<void*>(::dice::hfe::fileChangeManager))
            ->addListener(reinterpret_cast<io::IFileChangeListener*>(this), name, 4);
#else
        static_cast<IFileChangeManagerPe*>(static_cast<void*>(::dice::hfe::fileChangeManager))
            ->addListener(reinterpret_cast<IBase*>(this), name, 4);
#endif
    }

    raRelease(errors);

    loadStat->m_unknown08 = false;
    {
        ::dice::hfe::loadtrace::Scope eventTrace("fx.event", name.c_str());   // [DEV] BF2_LOAD_TRACE
        shaderLoadingEvent();                                 // PE 0x1003582C..0x100358A6
    }

    ::renddx9abi::engineFree(source);                          // sub_1005F360 on Str1
    return effect;
}


// Returns the cached effect or compiles it using the lower-case name.
// Updates the last-name/effect cache even when compilation returns null.

void* EffectManager::getEffect(const std::string& name)
{
    std::string key(name);
    pathToLower(key);

    void* effect = 0;

    // sub_10036390 - std::map::find with the `_stricmp` comparator. Its caller compares the
    // returned iterator against *(this + 0x30), the head, so a miss is end().
    const RaEffectCache::const_iterator node = m_effectCache.find(key);
    if (node != m_effectCache.end())
    {
        effect = node->second;
    }
    else
    {
        compileEffect(key, 0, &effect);
        if (effect)
        {
            // sub_100372D0 - operator[], which inserts on a miss and returns &node->second.
            m_effectCache[key] = effect;

            // Requires the published StateManager.
            static_cast<ID3DXEffect*>(effect)->SetStateManager(
                static_cast<LPD3DXEFFECTSTATEMANAGER>(g_stateManager));
        }
    }

    m_lastEffect = effect;                                     // +0x28
    m_name       = key;                                        // +0x0C
    return effect;
}


// THE Ra ID3DXInclude handler - off_101E139C, the sub-object's primary table.


namespace {


// [CHOSEN, not recovered] The pattern matched by hand when the file name holds no regex syntax but
// '.' (any character except '\n' and '\r', as in the regex): std::regex cost about 2 ms per effect
// request on macOS (every line of every include, thousands of requests per level load). Same
// result as std::regex_match of the line against the pattern.
bool raIncludeLineMatches(const char* line, const char* lineEnd, const char* fileName)
{
    static const char kDirective[] = "#include";
    const std::size_t directive = sizeof(kDirective) - 1;
    if (static_cast<std::size_t>(lineEnd - line) < directive || ::memcmp(line, kDirective, directive) != 0)
        return false;
    const char* p = line + directive;
    const char* const spaces = p;
    while (p < lineEnd && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' || *p == '\f' || *p == '\r'))
        ++p;
    if (p == spaces || p == lineEnd || *p++ != '"')
        return false;
    for (const char* n = fileName; *n; ++n, ++p)
        if (p == lineEnd || (*n == '.' ? (*p == '\n' || *p == '\r') : *p != *n))
            return false;
    if (p == lineEnd || *p++ != '"')
        return false;
    for (; p < lineEnd; ++p)
        if (*p == '\n' || *p == '\r')
            return false;
    return true;
}

// PME("^#include\s+\"<name>\".*", "mg") driven one line at a time. Ra/RaShaderPermutator.cpp
// records why PME itself is not here: it is a third-party pcre wrapper (?match@PME@@, ??apme@@,
// ?getMarkers@PME@@) that this tree does not carry, so the pattern is transcribed verbatim and the
// cursor is written out. `matchEnd` is where PME::getMarkers(0) would put the match's end - the
// byte before the newline, because `.` does not match '\n' but does match the '\r' of a CRLF.
bool raFindIncludeLine(const std::string& text, const char* fileName,
                       std::string::size_type& matchBegin, std::string::size_type& matchEnd)
{
    const bool literal = ::strpbrk(fileName, "\\^$*+?()[]{}|") == 0;
    const std::string pattern = literal ? std::string() : "^#include\\s+\"" + std::string(fileName) + "\".*";
    std::regex re;
    if (!literal) try
    {
        re.assign(pattern);
    }
    catch (const std::regex_error&)
    {
        // The shipped pattern embeds the requested file name unescaped, so a name carrying regex
        // punctuation is a pattern PME would accept and std::regex may not. Refusing here leaves
        // the expanded copy un-spliced, which only the (unreachable) ps_1_4 fixup would have read.
        ::renddx9abi::logf("Ra include: the transcribed #include pattern did not compile for "
                           "\"%s\"; the expanded copy is left alone", pattern.c_str());
        return false;
    }

    std::string::size_type from = 0;
    for (;;)
    {
        std::string::size_type rawEnd = text.find('\n', from);
        if (rawEnd == std::string::npos)
            rawEnd = text.size();
        std::string::size_type lineEnd = rawEnd;
        while (lineEnd > from && text[lineEnd - 1] == '\r')
            --lineEnd;

        if (literal ? raIncludeLineMatches(text.data() + from, text.data() + lineEnd, fileName)
                    : std::regex_match(text.begin() + static_cast<std::ptrdiff_t>(from),
                                       text.begin() + static_cast<std::ptrdiff_t>(lineEnd), re))
        {
            matchBegin = from;
            matchEnd   = rawEnd;
            return true;
        }

        if (rawEnd >= text.size())
            return false;
        from = rawEnd + 1;
    }
}


// PE 0x10037470.
// Supplies virtual RaDefines.fx or reads and watches an include, then splices its text
// into the expanded source. Always returns S_OK, including file failures; on open failure
// outBytes remains untouched and the allocated buffer is not released.
long __stdcall raIncludeOpen(void* self, int /*includeType*/, const char* fileName,
                             const void* /*parentData*/, const void** outData,
                             unsigned int* outBytes)
{
    EffectSubObject* const sub = static_cast<EffectSubObject*>(self);
    EffectManager* const  owner = static_cast<EffectManager*>(g_effectManager);

    // [CHOSEN, not recovered] Reject missing callback arguments or an unpublished owner.
    if (!fileName || !outData || !outBytes || !sub || !owner)
    {
        if (outData)  *outData  = 0;
        if (outBytes) *outBytes = 0;
        return 0;                                              // the shipped constant
    }

    if (::_stricmp(fileName, "shaders/RaDefines.fx") == 0
        || ::_stricmp(fileName, "mods/xpack/shaders/RaDefines.fx") == 0)
    {
        std::string defines(owner->shaderDefines());
        defines += owner->debugDefines();

        const unsigned int size = static_cast<unsigned int>(defines.size());   // a few KB of text
        *outBytes = size;
        char* const buffer = static_cast<char*>(::renddx9abi::engineAlloc(size + 1));
        *outData = buffer;
        if (buffer)
            ::strcpy(buffer, defines.c_str());
    }
    else
    {
        std::string path(fileName);
        if (g_Renderer)
            applyModPrefix(path);

        // Read the setting on every call before opening the file.
        const bool obfuscated = readBoolSetting("GSLoadObfuscated", true);

        RaFileInfo info;
        if (!raGetFileInfo(path, info))
        {
            // AND IT carries ON. 0x1003751D logs EffectManager.cpp:107 and then allocates
            // `info.size + 1` out of a record the probe never filled.
            ::renddx9abi::logf("EffectManager.cpp:107 Can't load effect file: %s", path.c_str());
        }

        char* const buffer = static_cast<char*>(::renddx9abi::engineAlloc(info.size + 1));
        *outData = buffer;

        renddx9abi::HostStream* const file = raOpen(path, 2, 0);
        if (!file)
        {
            ::renddx9abi::logf("EffectManager.cpp:114 Can't load effect file: %s", path.c_str());
            return 0;
        }

        *outBytes = buffer ? file->read(buffer, info.size) : 0;
        file->release();

        if (*outBytes != info.size)
        {
            ::renddx9abi::logf("EffectManager.cpp:122 Can't load whole file: %s", path.c_str());
            return 0;
        }

        buffer[info.size] = 0;
        if (obfuscated)
            deobfuscateShader(reinterpret_cast<unsigned char*>(buffer), info.size);

        raWatchInclude(sub, path);
    }

    // The splice. `"^#include\\s+\"" + fileName + "\".*"`, PME flags "mg", applied to the expanded
    // copy until no match remains.
    {
        for (;;)
        {
            // PE 0x10037878: basic_string(const char*) from c_str(), so the copy stops at an embedded NUL.
            std::string text(sub->text.c_str());
            std::string::size_type begin = 0;
            std::string::size_type end   = 0;
            if (!raFindIncludeLine(text, fileName, begin, end))
                break;

            const unsigned int bytes = *outBytes;
            const char* const  data  = static_cast<const char*>(*outData);

            std::string spliced;
            spliced.reserve(text.size() + bytes);
            spliced.append(text, 0, begin);
            if (data && bytes)
                spliced.append(data, bytes);
            spliced.append(text, end, text.size() - end);

            // std::string::operator=(char const*) at so an embedded NUL truncates
            // here in the original too.
            sub->text = spliced.c_str();
        }
    }

    return 0;
}

// sub_10037450, 0x10037450, 21 B - ID3DXInclude::Close, slot +0x04. `bf_free(pData); return 0;`
// with no null test of its own.
long __stdcall raIncludeClose(void* /*self*/, const void* data)
{
    ::renddx9abi::engineFree(const_cast<void*>(data));
    return 0;
}

void* const kRaIncludeVtable[2] =
{
    reinterpret_cast<void*>(&raIncludeOpen),     // +0x00
    reinterpret_cast<void*>(&raIncludeClose)     // +0x04
};

void* raIncludeVtable()
{
    return const_cast<void**>(kRaIncludeVtable);
}

} // anonymous namespace


// Shader-directory loading helpers.
// Flag bit 0 loads bare shaders/*.mfx; bit 1 scans mod-prefixed rashader*.fx;
// bit 2 additionally builds those shaders immediately. STM/SM/BM master shaders use the
// meta-effect path and are excluded from the ordinary scan.
// [PLATFORM] compatibility builds retain a reporting seam because the host file-list ABI
// is unavailable; native builds perform the scan.
namespace {

// sub_10041AC0 - see the block above.  The argument is already lowercased.
bool raIsLoadableShaderFx(const char* lowered)
{
    bool candidate = false;
    if (::strstr(lowered, "rashader") != 0 && ::strstr(lowered, ".fx") != 0)
        candidate = (::strstr(lowered, "common") == 0);

    return candidate
        && ::_stricmp(lowered, "rashaderSTM.fx") != 0
        && ::_stricmp(lowered, "rashaderSM.fx")  != 0
        && ::_stricmp(lowered, "rashaderBM.fx")  != 0;
}

} // anonymous namespace

int raLoadAllShaders(RaShaderTable* table, unsigned char flags)
{
    static_cast<EffectManager*>(g_effectManager)->resetEffects();

    {
        const std::string defaultName("default");
        table->defaultShader = findOrCreateShader(table, defaultName, false);   // +0x84
    }

    int created = 0;

    std::list< ::dice::hfe::io::FileInfo> files;

    if (::dice::hfe::fileManager)
    {
        const ::renddx9abi::HostString bare =
            ::renddx9abi::makeHostString(std::string("shaders/"));
        ::dice::hfe::fileManager->findFiles(bare, false, files);         // vslot +0x34
    }

    if ((flags & 1u) != 0)
    {
        for (std::list< ::dice::hfe::io::FileInfo>::const_iterator it = files.begin();
             it != files.end(); ++it)
        {
            // sub_10075180 on the entry name, then sub_100750C0 against std::string("mfx") over 3 chars.
            if (!equalsIgnoreCase(extractFileExt(it->name), std::string("mfx"), 3))
                continue;

            std::string full("shaders/");
            full += it->name.c_str();

            // The flag is the table's own +0xB4 - the same byte refillMemoryFiles reads one
            // indirection later to decide whether a MetaFx writes its generated files out.
            static_cast<EffectManager*>(g_effectManager)->loadMetaFx(full, table->byteB4 != 0);
        }
    }

    static_cast<EffectManager*>(g_effectManager)->refillMemoryFiles();

    std::string modRoot("shaders/");
    if (g_Renderer)
        applyModPrefix(modRoot);

    files.clear();                                                       // sub_10043B60(0, ...)
    if (::dice::hfe::fileManager)
    {
        const ::renddx9abi::HostString prefixed = ::renddx9abi::makeHostString(modRoot);
        ::dice::hfe::fileManager->findFiles(prefixed, false, files);
    }

    if ((flags & 2u) != 0)
    {
        for (std::list< ::dice::hfe::io::FileInfo>::const_iterator it = files.begin();
             it != files.end(); ++it)
        {
            // sub_10074E80 is `copy then sub_10074D10`, and sub_10074D10 is the pathToLower this
            // file already uses twice above; written out rather than reaching into RaShader.cpp's
            // file-local copy of the pair.
            std::string lowered(it->name.c_str());
            pathToLower(lowered);
            if (!raIsLoadableShaderFx(lowered.c_str()))
                continue;

            std::string stem;
            raCacheStem(stem, lowered);
            if (stem.size() < 8u)
                continue;                                                 // see the note above

            const std::string name(stem.c_str() + 8);                    // substr(8) - "rashader"
            RaShader* const shader = findOrCreateShader(table, name, false);

            if ((flags & 4u) != 0 && shader)
                shader->slot04();

            ++created;
        }
    }

    ::renddx9abi::logf("Ra: shaders/* load pass - %u file(s) scanned, %d Ra shader(s) created "
                       "(flags 0x%02X)",
                       static_cast<unsigned int>(files.size()), created,
                       static_cast<unsigned int>(flags));

    return created;
}


} // namespace Ra


} // namespace dx9rend

} // namespace hfe
} // namespace dice

#endif // BF2_IMAGE_RENDDX9
